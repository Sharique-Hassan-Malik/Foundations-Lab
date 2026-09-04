CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -Iinclude -Isrc
SRC      = src/bits.c src/huffman.c src/deflate.c src/inflate.c

.PHONY: all test gzcheck bench clean

all: test

test: $(SRC) tests/test_deflate.c
	$(CC) $(CFLAGS) $(SRC) tests/test_deflate.c -o test_deflate
	./test_deflate

# The headline claim: gunzip accepts what this produces. Needs gzip installed.
gzcheck: $(SRC) tools/gzdeflate.c
	$(CC) $(CFLAGS) $(SRC) tools/gzdeflate.c -o gzdeflate
	./tools/gzcheck.sh

bench: $(SRC) bench/bench.c
	$(CC) $(CFLAGS) $(SRC) bench/bench.c -o bench_deflate
	./bench_deflate

clean:
	rm -f test_deflate gzdeflate bench_deflate
