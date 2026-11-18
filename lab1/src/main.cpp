#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Count = std::uint64_t;
using Clock = std::chrono::steady_clock;

constexpr double kLeftBorder = 2.0;
constexpr double kRightBorder = 5.0;
constexpr Count kDefaultSubintervals = 1'000'000;
constexpr std::chrono::milliseconds kMinimumBenchmarkWindow{75};
constexpr std::size_t kMaximumBenchmarkRepetitions = 1U << 20;
constexpr std::size_t kBenchmarkSamples = 5;

struct Configuration {
    Count subintervals{};
    std::size_t main_threads{};
    std::size_t max_benchmark_threads{};
    std::filesystem::path csv_path{"results/benchmark_results.csv"};
    bool run_refinement{true};
};

struct CommandLineOptions {
    std::optional<Count> subintervals;
    std::optional<std::size_t> main_threads;
    std::optional<std::size_t> max_benchmark_threads;
    std::optional<std::filesystem::path> csv_path;
    bool non_interactive{false};
    bool skip_refinement{false};
    bool show_help{false};
};

struct IndexRange {
    Count begin{};  // Inclusive internal trapezoid index.
    Count end{};    // Exclusive internal trapezoid index.
};

struct Measurement {
    double seconds{};
    double result{};
    std::size_t repetitions{1};
};

struct BenchmarkRow {
    std::string technology;
    std::size_t threads{};
    double seconds{};
    double result{};
    double speedup{};
    double efficiency_percent{};
    std::size_t repetitions{};
};

struct RefinementResult {
    double epsilon{};
    Count subintervals{};
    double step{};
    double integral{};
    double runge_error{};
};

[[nodiscard]] std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

[[nodiscard]] bool parse_positive_count(const std::string& text, Count& value) {
    const std::string cleaned = trim(text);
    if (cleaned.empty() || cleaned.front() == '-') {
        return false;
    }

    unsigned long long parsed = 0;
    std::istringstream stream(cleaned);
    stream >> parsed;
    if (!stream || !stream.eof() || parsed == 0 ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<Count>::max())) {
        return false;
    }

    value = static_cast<Count>(parsed);
    return true;
}

[[nodiscard]] bool parse_positive_thread_count(const std::string& text, std::size_t& value) {
    Count parsed = 0;
    if (!parse_positive_count(text, parsed) ||
        parsed > static_cast<Count>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

[[nodiscard]] std::size_t hardware_thread_default() {
    const unsigned int detected = std::thread::hardware_concurrency();
    const std::size_t available = detected == 0 ? 2U : static_cast<std::size_t>(detected);
    return std::max<std::size_t>(1U, std::min<std::size_t>(available, 8U));
}

void print_help(const char* executable_name) {
    std::cout
        << "Использование: " << executable_name << " [параметры]\n\n"
        << "Параметры:\n"
        << "  --n <N>             число разбиений N > 0\n"
        << "  --threads <T>       число потоков для основного расчёта T >= 1\n"
        << "  --tmax <Tmax>       верхняя граница потоков benchmark Tmax >= 1\n"
        << "  --csv <путь>        CSV (по умолчанию results/benchmark_results.csv)\n"
        << "  --non-interactive   не запрашивать отсутствующие значения, взять defaults\n"
        << "  --skip-refinement   не выполнять отдельное уточнение для epsilon\n"
        << "  --help, -h          показать эту справку\n\n"
        << "Без всех трёх числовых параметров программа запросит недостающие\n"
        << "значения. Пустой ответ выбирает безопасное значение по умолчанию.\n";
}

[[nodiscard]] std::string require_option_value(int& index, int argc, char* argv[],
                                                const std::string& option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument("для параметра " + option + " не задано значение");
    }
    ++index;
    return argv[index];
}

[[nodiscard]] CommandLineOptions parse_command_line(int argc, char* argv[]) {
    CommandLineOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--n") {
            const std::string value = require_option_value(index, argc, argv, argument);
            Count parsed = 0;
            if (!parse_positive_count(value, parsed)) {
                throw std::invalid_argument("N должно быть целым числом больше нуля");
            }
            options.subintervals = parsed;
        } else if (argument == "--threads") {
            const std::string value = require_option_value(index, argc, argv, argument);
            std::size_t parsed = 0;
            if (!parse_positive_thread_count(value, parsed)) {
                throw std::invalid_argument("T должно быть целым числом не меньше 1");
            }
            options.main_threads = parsed;
        } else if (argument == "--tmax") {
            const std::string value = require_option_value(index, argc, argv, argument);
            std::size_t parsed = 0;
            if (!parse_positive_thread_count(value, parsed)) {
                throw std::invalid_argument("Tmax должно быть целым числом не меньше 1");
            }
            options.max_benchmark_threads = parsed;
        } else if (argument == "--csv") {
            const std::string value = require_option_value(index, argc, argv, argument);
            if (value.empty()) {
                throw std::invalid_argument("путь к CSV не может быть пустым");
            }
            options.csv_path = std::filesystem::path(value);
        } else if (argument == "--non-interactive") {
            options.non_interactive = true;
        } else if (argument == "--skip-refinement") {
            options.skip_refinement = true;
        } else {
            throw std::invalid_argument("неизвестный параметр: " + argument);
        }
    }

    return options;
}

template <typename Value, typename Parser>
[[nodiscard]] Value ask_for_value(const std::string& prompt, const Value& default_value,
                                  Parser parser) {
    while (true) {
        std::cout << prompt << " [по умолчанию " << default_value << "]: ";
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\nСтандартный ввод недоступен; выбрано значение по умолчанию.\n";
            return default_value;
        }

        line = trim(line);
        if (line.empty()) {
            return default_value;
        }

        Value parsed{};
        if (parser(line, parsed)) {
            return parsed;
        }
        std::cout << "Некорректное значение. Попробуйте ещё раз.\n";
    }
}

[[nodiscard]] Configuration obtain_configuration(const CommandLineOptions& options) {
    const std::size_t default_max_threads = hardware_thread_default();
    const std::size_t default_main_threads = std::min<std::size_t>(4U, default_max_threads);

    Configuration configuration{
        kDefaultSubintervals,
        default_main_threads,
        default_max_threads,
        "results/benchmark_results.csv",
        !options.skip_refinement,
    };

    const bool prompt_for_missing_values = !options.non_interactive;

    if (options.subintervals) {
        configuration.subintervals = *options.subintervals;
    } else if (prompt_for_missing_values) {
        configuration.subintervals = ask_for_value<Count>(
            "Введите N — число разбиений", configuration.subintervals, parse_positive_count);
    }

    if (options.main_threads) {
        configuration.main_threads = *options.main_threads;
    } else if (prompt_for_missing_values) {
        configuration.main_threads = ask_for_value<std::size_t>(
            "Введите T — число потоков основного расчёта", configuration.main_threads,
            parse_positive_thread_count);
    }

    if (options.max_benchmark_threads) {
        configuration.max_benchmark_threads = *options.max_benchmark_threads;
    } else if (prompt_for_missing_values) {
        configuration.max_benchmark_threads = ask_for_value<std::size_t>(
            "Введите Tmax — максимум потоков benchmark", configuration.max_benchmark_threads,
            parse_positive_thread_count);
    }

    if (options.csv_path) {
        configuration.csv_path = *options.csv_path;
    }

    return configuration;
}

// Function variant 3: f(x) = (1 + x)^2 / (x^3 * sqrt(2 + x)).
[[nodiscard]] double integrand(double x) {
    const double numerator = (1.0 + x) * (1.0 + x);
    const double denominator = x * x * x * std::sqrt(2.0 + x);
    return numerator / denominator;
}

[[nodiscard]] double step_for(Count subintervals) {
    return (kRightBorder - kLeftBorder) / static_cast<double>(subintervals);
}

[[nodiscard]] double sum_internal_points(Count begin, Count end, double step) {
    double sum = 0.0;
    for (Count index = begin; index < end; ++index) {
        sum += integrand(kLeftBorder + static_cast<double>(index) * step);
    }
    return sum;
}

[[nodiscard]] double assemble_trapezoids(Count subintervals, double internal_sum) {
    const double step = step_for(subintervals);
    const double border_sum = (integrand(kLeftBorder) + integrand(kRightBorder)) / 2.0;
    return step * (border_sum + internal_sum);
}

// Sequential baseline for the trapezoidal rule.
[[nodiscard]] double integrate_sequential(Count subintervals) {
    const double step = step_for(subintervals);
    const double internal_sum = sum_internal_points(1, subintervals, step);
    return assemble_trapezoids(subintervals, internal_sum);
}

[[nodiscard]] IndexRange make_range(Count subintervals, std::size_t worker_index,
                                    std::size_t worker_count) {
    const Count internal_points = subintervals > 1 ? subintervals - 1 : 0;
    const Count workers = static_cast<Count>(worker_count);
    const Count index = static_cast<Count>(worker_index);

    const Count base_size = internal_points / workers;
    const Count remainder = internal_points % workers;
    const Count current_size = base_size + (index < remainder ? 1U : 0U);
    const Count begin = 1U + index * base_size + std::min(index, remainder);

    return {begin, begin + current_size};
}

struct PthreadWorkerArguments {
    Count begin{};
    Count end{};
    double step{};
};

// The heap allocation intentionally outlives the worker.  The caller takes
// ownership after pthread_join and releases it with std::unique_ptr.
void* pthread_worker(void* raw_arguments) {
    const auto* arguments = static_cast<const PthreadWorkerArguments*>(raw_arguments);
    try {
        auto partial_sum = std::make_unique<double>(
            sum_internal_points(arguments->begin, arguments->end, arguments->step));
        return partial_sum.release();
    } catch (...) {
        // A null return value is checked by the joining thread and reported as an error.
        return nullptr;
    }
}

[[nodiscard]] std::string pthread_error_text(int error_code) {
    return std::strerror(error_code);
}

[[nodiscard]] double collect_pthread_results(std::vector<pthread_t>& workers,
                                             std::size_t created_workers) {
    int first_join_error = 0;
    bool worker_returned_null = false;
    double sum = 0.0;

    // Join every thread even if one join reports an error.  Ownership of a
    // successful worker's raw return value is adopted immediately, so no extra
    // allocation is required after workers have been started.
    for (std::size_t index = 0; index < created_workers; ++index) {
        void* return_value = nullptr;
        const int status = pthread_join(workers[index], &return_value);
        if (status != 0 && first_join_error == 0) {
            first_join_error = status;
        }
        if (status == 0) {
            std::unique_ptr<double> partial_sum(static_cast<double*>(return_value));
            if (!partial_sum) {
                worker_returned_null = true;
            } else {
                sum += *partial_sum;
            }
        }
    }

    if (first_join_error != 0) {
        throw std::runtime_error("pthread_join завершился с ошибкой: " +
                                 pthread_error_text(first_join_error));
    }

    if (worker_returned_null) {
        throw std::runtime_error("pthread worker не смог выделить память для partial sum");
    }
    return sum;
}

// POSIX Threads version.  Its partial results are transferred through the
// pthread return value, not through a shared mutable accumulator.
[[nodiscard]] double integrate_pthread(Count subintervals, std::size_t worker_count) {
    const double step = step_for(subintervals);
    std::vector<pthread_t> workers(worker_count);
    std::vector<PthreadWorkerArguments> arguments(worker_count);

    std::size_t created_workers = 0;
    int create_error = 0;
    for (std::size_t index = 0; index < worker_count; ++index) {
        const IndexRange range = make_range(subintervals, index, worker_count);
        arguments[index] = {range.begin, range.end, step};
        const int status = pthread_create(&workers[index], nullptr, pthread_worker, &arguments[index]);
        if (status != 0) {
            create_error = status;
            break;
        }
        ++created_workers;
    }

    const double internal_sum = collect_pthread_results(workers, created_workers);
    if (create_error != 0) {
        throw std::runtime_error("pthread_create завершился с ошибкой: " +
                                 pthread_error_text(create_error));
    }

    return assemble_trapezoids(subintervals, internal_sum);
}

// std::thread receives each worker argument separately.  In particular, there
// is no aggregate argument structure passed to std::thread.
void std_thread_worker(Count begin, Count end, double step, double* destination) {
    *destination = sum_internal_points(begin, end, step);
}

[[nodiscard]] double integrate_std_thread(Count subintervals, std::size_t worker_count) {
    const double step = step_for(subintervals);
    std::vector<double> partial_sums(worker_count, 0.0);
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            const IndexRange range = make_range(subintervals, index, worker_count);
            workers.emplace_back(std_thread_worker, range.begin, range.end, step,
                                 &partial_sums[index]);
        }
    } catch (...) {
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    double internal_sum = 0.0;
    for (const double partial_sum : partial_sums) {
        internal_sum += partial_sum;
    }
    return assemble_trapezoids(subintervals, internal_sum);
}

template <typename Calculation>
[[nodiscard]] Measurement measure_once(Calculation&& calculation) {
    const auto started = Clock::now();
    const double result = calculation();
    const auto finished = Clock::now();
    return {std::chrono::duration<double>(finished - started).count(), result, 1};
}

// Repeats only real calculations.  The representative value is a median of
// several timed batches, so a scheduler interruption does not decide the row.
// No sleeps or synthetic timing values are used.
template <typename Calculation>
[[nodiscard]] Measurement benchmark_repeated(Calculation&& calculation) {
    volatile double sink = calculation();  // one unmeasured warm-up call
    std::size_t repetitions = 1;

    // Calibrate a real amount of work long enough for clock resolution and
    // thread-creation noise not to dominate one sample.
    while (true) {
        const auto started = Clock::now();
        for (std::size_t iteration = 0; iteration < repetitions; ++iteration) {
            sink = calculation();
        }
        const auto finished = Clock::now();
        if (finished - started >= kMinimumBenchmarkWindow ||
            repetitions >= kMaximumBenchmarkRepetitions) {
            break;
        }
        repetitions *= 2U;
    }

    // All samples are independently measured real calls.  The median avoids
    // silently selecting a favourable single batch while remaining robust to
    // occasional system scheduling noise.
    std::vector<double> seconds_per_calculation;
    seconds_per_calculation.reserve(kBenchmarkSamples);
    for (std::size_t sample = 0; sample < kBenchmarkSamples; ++sample) {
        const auto started = Clock::now();
        for (std::size_t iteration = 0; iteration < repetitions; ++iteration) {
            sink = calculation();
        }
        const auto finished = Clock::now();
        const double elapsed = std::chrono::duration<double>(finished - started).count();
        seconds_per_calculation.push_back(elapsed / static_cast<double>(repetitions));
    }

    auto median = seconds_per_calculation.begin() +
                  static_cast<std::ptrdiff_t>(seconds_per_calculation.size() / 2U);
    std::nth_element(seconds_per_calculation.begin(), median, seconds_per_calculation.end());
    return {*median, static_cast<double>(sink), repetitions};
}

template <typename Calculation>
[[nodiscard]] std::vector<BenchmarkRow> benchmark_technology(const std::string& technology,
                                                              Count subintervals,
                                                              std::size_t max_threads,
                                                              Calculation calculation) {
    std::vector<BenchmarkRow> rows;
    rows.reserve(max_threads);

    for (std::size_t thread_count = 1;; ++thread_count) {
        const Measurement measurement = benchmark_repeated(
            [&] { return calculation(subintervals, thread_count); });
        rows.push_back({technology, thread_count, measurement.seconds, measurement.result,
                        0.0, 0.0, measurement.repetitions});
        if (thread_count == max_threads) {
            break;
        }
    }

    const double one_thread_time = rows.front().seconds;
    for (BenchmarkRow& row : rows) {
        row.speedup = row.seconds > 0.0 ? one_thread_time / row.seconds : 0.0;
        row.efficiency_percent = row.speedup / static_cast<double>(row.threads) * 100.0;
    }
    return rows;
}

[[nodiscard]] RefinementResult refine_until(double epsilon) {
    Count coarse_subintervals = 1;
    double coarse_integral = integrate_sequential(coarse_subintervals);

    while (true) {
        if (coarse_subintervals > std::numeric_limits<Count>::max() / 2U) {
            throw std::overflow_error("при уточнении достигнут предел числа разбиений");
        }

        const Count refined_subintervals = coarse_subintervals * 2U;
        const double refined_integral = integrate_sequential(refined_subintervals);
        const double runge_error = std::abs(refined_integral - coarse_integral) / 3.0;

        if (runge_error <= epsilon) {
            return {epsilon, refined_subintervals, step_for(refined_subintervals),
                    refined_integral, runge_error};
        }

        coarse_subintervals = refined_subintervals;
        coarse_integral = refined_integral;
    }
}

void print_main_measurement(const std::string& title, const Measurement& measurement,
                            double difference_from_sequential) {
    std::cout << std::left << std::setw(18) << title << std::right
              << " I = " << std::setprecision(12) << measurement.result
              << ", time = " << std::setprecision(9) << measurement.seconds << " s"
              << ", |delta| = " << std::setprecision(12) << difference_from_sequential << '\n';
}

void print_refinement_result(const RefinementResult& result) {
    std::cout << "epsilon = " << std::scientific << std::setprecision(0) << result.epsilon
              << std::fixed << std::setprecision(0)
              << ": N = " << result.subintervals
              << ", h = " << std::setprecision(12) << result.step
              << ", I = " << result.integral
              << ", оценка Рунге = " << std::scientific << std::setprecision(6)
              << result.runge_error << std::fixed << '\n';
}

void print_benchmark_table(const std::string& technology,
                           const std::vector<BenchmarkRow>& rows) {
    std::cout << "\nBenchmark: " << technology << "\n"
              << "  p       time, s              I             S(p)      E(p), %   repeats\n";
    for (const BenchmarkRow& row : rows) {
        std::cout << std::setw(3) << row.threads << "  "
                  << std::setw(14) << std::setprecision(9) << row.seconds << "  "
                  << std::setw(14) << std::setprecision(10) << row.result << "  "
                  << std::setw(8) << std::setprecision(4) << row.speedup << "  "
                  << std::setw(8) << std::setprecision(4) << row.efficiency_percent << "  "
                  << row.repetitions << '\n';
    }
}

void write_csv(const std::filesystem::path& path,
               const std::vector<BenchmarkRow>& pthread_rows,
               const std::vector<BenchmarkRow>& std_thread_rows) {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("не удалось открыть CSV для записи: " + path.string());
    }

    output << "technology,threads,time_seconds,result,speedup,efficiency_percent,repetitions\n";
    output << std::setprecision(17);

    const auto write_row = [&output](const BenchmarkRow& row) {
        output << row.technology << ',' << row.threads << ',' << row.seconds << ','
               << row.result << ',' << row.speedup << ',' << row.efficiency_percent << ','
               << row.repetitions << '\n';
    };

    for (const BenchmarkRow& row : pthread_rows) {
        write_row(row);
    }
    for (const BenchmarkRow& row : std_thread_rows) {
        write_row(row);
    }

    if (!output) {
        throw std::runtime_error("ошибка при записи CSV: " + path.string());
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const CommandLineOptions options = parse_command_line(argc, argv);
        if (options.show_help) {
            print_help(argv[0]);
            return 0;
        }

        const Configuration configuration = obtain_configuration(options);
        std::cout << std::fixed;
        std::cout << "Метод трапеций, f(x) = (1+x)^2 / (x^3 * sqrt(2+x)), [2, 5]\n"
                  << "N = " << configuration.subintervals
                  << ", T = " << configuration.main_threads
                  << ", Tmax = " << configuration.max_benchmark_threads << "\n\n";

        // This is the separate main calculation for the user-provided N and T.
        const Measurement sequential_main = measure_once(
            [&] { return integrate_sequential(configuration.subintervals); });
        const Measurement pthread_main = measure_once(
            [&] { return integrate_pthread(configuration.subintervals, configuration.main_threads); });
        const Measurement std_thread_main = measure_once(
            [&] { return integrate_std_thread(configuration.subintervals, configuration.main_threads); });

        std::cout << "Основной расчёт (время включает только вычисление):\n";
        print_main_measurement("Последовательно", sequential_main, 0.0);
        print_main_measurement("POSIX Threads", pthread_main,
                               std::abs(pthread_main.result - sequential_main.result));
        print_main_measurement("std::thread", std_thread_main,
                               std::abs(std_thread_main.result - sequential_main.result));

        if (configuration.run_refinement) {
            std::cout << "\nПоследовательное уточнение сетки и оценка Рунге:\n";
            print_refinement_result(refine_until(1e-2));
            print_refinement_result(refine_until(1e-3));
        }

        std::cout << "\nРеальный benchmark: серия калибруется до "
                  << kMinimumBenchmarkWindow.count() << " ms, затем сохраняется медиана "
                  << kBenchmarkSamples
                  << " независимых серий реальных вычислений (или действует защитный лимит).\n";

        const std::vector<BenchmarkRow> pthread_rows = benchmark_technology(
            "pthread", configuration.subintervals, configuration.max_benchmark_threads,
            [](Count subintervals, std::size_t thread_count) {
                return integrate_pthread(subintervals, thread_count);
            });
        const std::vector<BenchmarkRow> std_thread_rows = benchmark_technology(
            "std_thread", configuration.subintervals, configuration.max_benchmark_threads,
            [](Count subintervals, std::size_t thread_count) {
                return integrate_std_thread(subintervals, thread_count);
            });

        print_benchmark_table("pthread", pthread_rows);
        print_benchmark_table("std::thread", std_thread_rows);

        write_csv(configuration.csv_path, pthread_rows, std_thread_rows);
        std::cout << "\nРеальные результаты benchmark сохранены: "
                  << configuration.csv_path.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Ошибка: " << error.what() << '\n';
        std::cerr << "Используйте --help для справки.\n";
        return 1;
    }
}
