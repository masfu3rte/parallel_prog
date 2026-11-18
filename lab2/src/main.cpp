#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using Integer = std::int64_t;
using Natural = std::uint64_t;

struct Task {
    std::size_t id;
    Integer a;
    Integer b;
};

struct Options {
    std::filesystem::path input = "tasks.txt";
    std::filesystem::path output = "output";
    std::size_t max_active = 8;
    std::string scenario = "real";
    bool help = false;
};

struct Delays {
    unsigned read_ms = 0;
    unsigned work_ms = 0;
    unsigned extra_first_ms = 0;
};

void pause(unsigned milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class T>
T parse_integer(const std::string& text) {
    T value{};
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error("некорректное целое число: " + text);
    }
    return value;
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument != "--input" && argument != "--output" &&
            argument != "--max-active" && argument != "--scenario") {
            throw std::runtime_error("неизвестный параметр: " + argument);
        }
        if (++i == argc) {
            throw std::runtime_error("нет значения для " + argument);
        }
        const std::string value = argv[i];
        if (argument == "--input") options.input = value;
        if (argument == "--output") options.output = value;
        if (argument == "--scenario") options.scenario = value;
        if (argument == "--max-active") {
            options.max_active = parse_integer<std::size_t>(value);
            if (options.max_active == 0 || options.max_active > 1024) {
                throw std::runtime_error(
                    "--max-active должен быть в диапазоне 1..1024");
            }
        }
    }
    return options;
}

Delays scenario_delays(const std::string& scenario) {
    if (scenario == "real") return {};
    if (scenario == "normal") return {20, 80, 0};
    if (scenario == "slow-manager") return {160, 30, 0};
    if (scenario == "slow-worker") return {20, 30, 650};
    throw std::runtime_error(
        "сценарий: real, normal, slow-manager или slow-worker");
}

// Conversion also works for INT64_MIN, whose positive value is not an int64_t.
Natural magnitude(Integer value) {
    return value < 0 ? static_cast<Natural>(-(value + 1)) + 1
                     : static_cast<Natural>(value);
}

Natural gcd(Integer first, Integer second) {
    Natural a = magnitude(first);
    Natural b = magnitude(second);
    while (b != 0) {
        const Natural remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

class Logger {
public:
    void event(const std::string& actor, std::size_t id, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start_).count();
        std::cout << "t=" << std::setw(4) << elapsed << " ms " << actor
                  << " task=" << id << ' ' << text << std::endl;
    }

private:
    Clock::time_point start_ = Clock::now();
    std::mutex mutex_;
};

std::string result_name(std::size_t id) {
    std::ostringstream name;
    name << "task_" << std::setfill('0') << std::setw(6) << id << ".txt";
    return name.str();
}

void solve(Task task, const std::filesystem::path& directory,
           Delays delays, Logger& log) {
    log.event("worker", task.id, "start");
    pause(delays.work_ms + (task.id == 1 ? delays.extra_first_ms : 0));
    const Natural result = gcd(task.a, task.b);
    log.event("worker", task.id, "gcd=" + std::to_string(result));
    const auto path = directory / result_name(task.id);
    std::ofstream output;
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.open(path);
    output << "task=" << task.id << '\n'
           << "a=" << task.a << '\n'
           << "b=" << task.b << '\n'
           << "gcd=" << result << '\n';
    output.close();
    log.event("worker", task.id, "saved " + path.filename().string());
}

// Every Entry owns a NEW thread for ONE task. There is no reusable worker pool.
class Executions {
    struct Entry {
        std::thread thread;
        std::size_t id = 0;
        bool finished = false;  // Access only under mutex_.
        std::exception_ptr error;  // Read only after join().
    };

public:
    explicit Executions(Logger& log) : log_(log) {}

    ~Executions() {
        for (auto& entry : entries_) {
            if (entry.thread.joinable()) entry.thread.join();
        }
    }

    void start(Task task, const Options& options, Delays delays, Logger& log) {
        reap(false);
        if (entries_.size() >= options.max_active) {
            log.event("manager", task.id, "wait for a free slot");
            reap(true);
        }
        // list guarantees stable addresses while other threads are created.
        entries_.emplace_back();
        Entry* entry = &entries_.back();
        entry->id = task.id;
        try {
            log.event("manager", task.id, "dispatch");
            entry->thread = std::thread([this, entry, task, delays, &options, &log] {
                try {
                    solve(task, options.output, delays, log);
                } catch (...) {
                    entry->error = std::current_exception();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    entry->finished = true;
                }
                completed_.notify_one();
            });
        } catch (...) {
            entries_.pop_back();
            throw;
        }
    }

    void finish() {
        while (!entries_.empty()) reap(true);
    }

private:
    void reap(bool wait) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (wait) {
            completed_.wait(lock, [this] {
                return std::any_of(entries_.begin(), entries_.end(),
                                   [](const Entry& entry) { return entry.finished; });
            });
        }
        std::exception_ptr error;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!it->finished) {
                ++it;
                continue;
            }
            // Worker does not lock mutex_ again after publishing finished.
            it->thread.join();
            log_.event("manager", it->id, "joined");
            if (!error && it->error) error = it->error;
            it = entries_.erase(it);
        }
        if (error) std::rethrow_exception(error);
    }

    std::mutex mutex_;
    std::condition_variable completed_;
    std::list<Entry> entries_;  // Modified only by the manager.
    Logger& log_;
};

std::size_t run(const Options& options, Delays delays, Logger& log) {
    std::ifstream input(options.input);
    if (!input) {
        throw std::runtime_error(
            "не удалось открыть " + options.input.string());
    }
    if (!std::filesystem::create_directories(options.output) &&
        !std::filesystem::is_empty(options.output)) {
        throw std::runtime_error(
            "каталог результатов должен быть новым или пустым");
    }

    Executions executions(log);
    std::string line;
    std::size_t line_number = 0;
    std::size_t count = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream fields(line);
        std::string a, b, extra;
        if (!(fields >> a)) continue;
        if (!(fields >> b) || (fields >> extra)) {
            throw std::runtime_error(
                "строка " + std::to_string(line_number) +
                ": нужны ровно два целых числа");
        }
        Task task{count + 1, 0, 0};
        try {
            task.a = parse_integer<Integer>(a);
            task.b = parse_integer<Integer>(b);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "строка " + std::to_string(line_number) +
                ": " + error.what());
        }
        pause(delays.read_ms);
        executions.start(task, options, delays, log);
        ++count;
    }
    if (input.bad()) {
        throw std::runtime_error("ошибка чтения входного файла");
    }
    executions.finish();
    return count;
}
}  // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            std::cout << "Лабораторная 2: мд1, НОД; C++17 std::thread\n"
                      << "--input PATH       пары int64 на отдельных строках\n"
                      << "--output DIR       новый или пустой каталог\n"
                      << "--max-active N     предел потоков, 1..1024 (8)\n"
                      << "--scenario NAME    real (по умолчанию), normal,\n"
                      << "                   slow-manager, slow-worker\n";
            return 0;
        }
        const Delays delays = scenario_delays(options.scenario);
        std::cout << "Model: md1; task: gcd; scenario: " << options.scenario
                  << "; max-active: " << options.max_active << '\n'
                  << "Delays (ms): read=" << delays.read_ms
                  << ", work=" << delays.work_ms
                  << ", extra task 1=" << delays.extra_first_ms << '\n';
        Logger log;
        const auto started = Clock::now();
        const std::size_t count = run(options, delays, log);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started).count();
        std::cout << "Completed: " << count << " tasks; elapsed: " << elapsed
                  << " ms; output: " << options.output.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ошибка: " << error.what() << '\n';
        return 1;
    }
}
