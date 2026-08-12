CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17 -pthread -Iinclude

.PHONY: all test bench clean

all: test

test: tests/test_workstealing.cpp include/workstealing.hpp
	$(CXX) $(CXXFLAGS) tests/test_workstealing.cpp -o test_ws
	./test_ws

bench: bench/bench.cpp include/workstealing.hpp
	$(CXX) $(CXXFLAGS) bench/bench.cpp -o bench_ws
	./bench_ws

clean:
	rm -f test_ws bench_ws
