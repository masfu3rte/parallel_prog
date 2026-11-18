CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
TARGET := build/lab2

.PHONY: all run clean
all: $(TARGET)

$(TARGET): src/main.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -pthread $< -o $@

run: all
	./$(TARGET)

clean:
	rm -rf build
