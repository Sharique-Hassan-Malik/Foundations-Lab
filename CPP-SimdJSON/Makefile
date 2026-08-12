CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17 -mavx2 -Iinclude

.PHONY: all test bench clean

all: test

test: build/test_simdjson
	./build/test_simdjson

bench: build/bench
	./build/bench

build/test_simdjson: tests/test_simdjson.cpp include/simdjson.hpp | build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_simdjson.cpp

build/bench: bench/bench.cpp include/simdjson.hpp | build
	$(CXX) $(CXXFLAGS) -o $@ bench/bench.cpp

build:
	mkdir -p build

clean:
	rm -rf build
