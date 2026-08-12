# C Memory Allocator (balloc)

A memory allocator — `malloc` / `free` / `realloc` / `calloc` — written from scratch in C, using a **segregated explicit free list with boundary-tag coalescing**, backed by `mmap`. The kind of allocator that sits underneath every C program, built and, crucially, *verified*.

## The headline: provably not corrupting memory

An allocator's one job is to never hand out overlapping memory and never lose track of a block. That is checked here as a number. A stress harness runs **2,000,000 random `malloc`/`free`/`realloc` operations**, writing a unique signature into every live allocation and re-checking it on every touch — any overlap or header/footer corruption flips a byte and is caught. Between operations it runs `balloc_check()`, which walks the entire heap and asserts every structural invariant.

```
$ make test

unit tests
  (0 failures so far)
stress: 2000000 random ops over 4096 live slots
  0 corruptions, 40 heap audits passed, 0 bytes still in use

ALL TESTS PASSED
```

**Zero corruptions**, every heap audit consistent, and after freeing everything the accounting returns to exactly **0 bytes in use** — no leaks. The invariants checked on every audit: 16-byte payload alignment, header equals footer on every block, no two physically adjacent free blocks (coalescing is complete), every free block is on its size-class list, and nothing allocated is.

## Performance

```
$ make bench

  balloc:        0.238 s  (12.6 Mops/s)
  system malloc: 0.109 s  (27.5 Mops/s)
  balloc / system ratio: 2.18x

  at 4096 live blocks: 1.53 MiB payload in 2.00 MiB heap (76.3% utilisation)
```

On a random workload it runs within about 2× of glibc's `malloc` — which is a heavily tuned, thread-caching production allocator — while keeping **76% of the heap as usable payload** (the remainder is the one-word header and footer per block, plus fragmentation). The goal here is a *correct, understandable* allocator, not to beat glibc; landing within a small constant factor of it is the bonus.

## How it works

Every block carries an 8-byte **header and a matching footer**, each holding the block's size with the low bit as an allocated flag. The duplicated footer is what makes coalescing O(1): to free a block and merge it with its left neighbour, you read the word just before the header — that's the neighbour's footer, giving its size and status. Free blocks additionally thread two pointers through the front of their payload, joining one of 16 **segregated free lists** chosen by size class, so allocation finds a fit without scanning everything. Memory is requested from the OS in `mmap`'d chunks, each framed by a prologue/epilogue so coalescing never runs off the ends.

```c
#include "balloc.h"

void *p = balloc(100);
p = brealloc(p, 4000);      // preserves the first 100 bytes
bfree(p);

balloc_stats s; balloc_stats_get(&s);   // bytes_in_use, heap_bytes, utilisation…
int ok = balloc_check();                // 0 if every invariant holds
```

## Build & run

```bash
make test     # build the library + run unit and stress tests
make bench     # build + run the throughput/fragmentation benchmark
make clean
```

Only a C11 compiler and a POSIX `mmap` are required (tested with gcc 13 on Linux).

## Layout

| path | what it holds |
|---|---|
| `include/balloc.h` | the public API and the stats/introspection hooks |
| `src/balloc.c` | the allocator: size classes, coalescing, `mmap` chunks, `balloc_check` |
| `tests/test_balloc.c` | unit checks + the 2M-operation signature-verifying stress harness |
| `bench/bench.c` | throughput vs system `malloc`, and a fragmentation figure |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the block layout, the boundary-tag coalescing cases, and every invariant `balloc_check` enforces.

## License

MIT — see [`LICENSE`](./LICENSE).
