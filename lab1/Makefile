CXX ?= c++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Wpedantic
THREAD_FLAGS ?= -pthread

TARGET := build/parallel_lab
SOURCE := src/main.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCE) | build
	$(CXX) $(CXXFLAGS) $(SOURCE) -o $(TARGET) $(THREAD_FLAGS)

build:
	mkdir -p build

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)
