CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -Iinclude
SRC      = src/coroutine.c src/switch.S

.PHONY: all test bench clean

all: test

test: $(SRC) tests/test_coroutine.c
	$(CC) $(CFLAGS) $(SRC) tests/test_coroutine.c -o test_coroutine
	./test_coroutine

bench: $(SRC) bench/bench.c
	$(CC) $(CFLAGS) $(SRC) bench/bench.c -o bench_coroutine
	./bench_coroutine

clean:
	rm -f test_coroutine bench_coroutine
