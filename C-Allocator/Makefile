CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -Iinclude
SRC      = src/balloc.c

.PHONY: all test bench clean

all: test

test: $(SRC) tests/test_balloc.c
	$(CC) $(CFLAGS) $(SRC) tests/test_balloc.c -o test_balloc
	./test_balloc

bench: $(SRC) bench/bench.c
	$(CC) $(CFLAGS) $(SRC) bench/bench.c -o bench_balloc
	./bench_balloc

clean:
	rm -f test_balloc bench_balloc
