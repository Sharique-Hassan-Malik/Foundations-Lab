# Architecture

The allocator is the classic segregated-fit design (as in CS:APP's malloc lab), specialised to an `mmap`-backed, multi-chunk heap. Three ideas carry it: boundary tags for O(1) coalescing, segregated free lists for fast fits, and a full invariant checker so "correct" is testable.

## Block layout

Every block, allocated or free, looks like this (a *word* is 8 bytes on LP64):

```
   header (8B)            payload                     footer (8B)
  +------------+----------------------------------+------------+
  |  size|flag |  ...                             |  size|flag |
  +------------+----------------------------------+------------+
  ^                                               
  bp - 8      bp (payload, 16-byte aligned)
```

The header and footer both store the block's **total size** (header + payload + footer) with the low bit as the allocated flag; because size is a multiple of 8, the low three bits are free for flags. A *free* block additionally stores two pointers — `next` and `prev` — at the very start of its payload, so it can live on a doubly linked list without any side table. The minimum block size (32 bytes) is exactly what's needed to hold a header, footer, and those two pointers.

Payloads are 16-byte aligned. A request for `n` bytes becomes a block of `align_up(n + 16, 16)` (the `+16` is header + footer), never smaller than 32.

## Boundary-tag coalescing

The footer is the trick that makes freeing cheap. Given a block's payload `bp`:

- its header is at `bp - 8`, and `NEXT_BLKP(bp) = bp + size` reaches the next block's payload;
- the **previous** block's footer sits at `bp - 16`, and reading its size gives `PREV_BLKP(bp) = bp - prev_size` — the left neighbour's payload, in O(1), with no traversal.

So `coalesce` inspects the allocated bits of both neighbours and handles four cases: neither free (nothing to do), only the next free (absorb it rightward), only the previous free (absorb leftward), or both free (absorb both). In each case the absorbed neighbours are unlinked from their free lists and the merged block's header and footer are rewritten to the combined size. Coalescing on every free is what guarantees the invariant "no two physically adjacent free blocks," which in turn bounds external fragmentation.

The free-block accounting lives with this: absorbing a neighbour decrements the free-block count and its payload from the running totals, and the caller re-adds the merged block once — so `bytes_in_use`/`bytes_free` stay exact. (Getting that wrong on the coalesce path was the one bug the unit tests caught; the stress test's byte-accounting assertion guards it now.)

## Segregated free lists

Free blocks are bucketed into 16 **size classes** — class `c` roughly covers `[32·2^c, 32·2^{c+1})` bytes, computed by shifting. Each class is the head of a doubly linked list. To allocate `asize`, `find_fit` scans from the smallest class that could contain it upward, first-fit within each class; because a class only holds blocks in a narrow size band, the first fit is usually a good fit and the scan is short. This is the compromise between a single free list (simple, but O(n) scans) and a full best-fit search (tight packing, but slow).

When a fit is found, `place` splits it: if the leftover after carving out `asize` is at least a minimum block, the remainder becomes a new free block on its own class list; otherwise the whole block is handed out (a few wasted bytes beat an unusable sliver).

## Growing the heap

When no class has a fit, `grow` asks the OS for a fresh chunk via `mmap` — `max(1 MiB, request)` rounded up. Each chunk is framed: a one-word allocated **prologue** footer at the front and an allocated, zero-size **epilogue** header at the very end. These sentinels mean the coalescing and heap-walk code never has to special-case the boundaries — the neighbour of the first real block always reads as "allocated," and the walk stops when it reaches a zero-size (epilogue) header. Chunks are threaded on a list so `balloc_reset` can `munmap` them all.

## The invariant checker

`balloc_check` is what turns "I think it's correct" into a test. It walks every block of every chunk (header to epilogue) and verifies:

1. the payload is 16-byte aligned;
2. the header equals the footer (size and flag agree — the cheapest corruption tripwire);
3. no block is below the minimum size;
4. every free block is actually on a free list, and no free block is immediately followed by another free block (coalescing is complete);

then scans the free lists themselves to confirm nothing on them is marked allocated. It returns a distinct negative code for the first violation, so a failing stress run points straight at which invariant broke. The stress harness calls it every 50,000 operations, and the payload-signature check runs on every single one — together they make a corrupting bug essentially impossible to miss.
