/* balloc — a from-scratch memory allocator.
 *
 * A drop-in-style malloc/free/realloc/calloc built on a segregated explicit
 * free list with boundary-tag coalescing, backed by mmap'd chunks. The design
 * is the classic one from CS:APP: every block carries a header and a matching
 * footer so that freeing a block can find and merge its physical neighbours in
 * O(1); free blocks of similar size are threaded onto per-size-class doubly
 * linked lists so allocation finds a fit quickly.
 *
 * The point of the project is that "correct" is checkable: `balloc_check` walks
 * the whole heap and asserts every invariant (alignment, header/footer
 * agreement, no two adjacent free blocks, free-list membership), and the test
 * harness pounds the allocator with millions of random operations while
 * verifying payload integrity. */
#ifndef BALLOC_H
#define BALLOC_H

#include <stddef.h>

void *balloc(size_t size);            /* like malloc */
void  bfree(void *ptr);               /* like free   */
void *brealloc(void *ptr, size_t size);
void *bcalloc(size_t nmemb, size_t size);

/* Heap introspection, for tests and benchmarks. */
typedef struct {
    size_t bytes_in_use;      /* payload bytes currently handed out */
    size_t bytes_free;        /* payload bytes available in free blocks */
    size_t heap_bytes;        /* total bytes mmap'd from the OS */
    size_t blocks_in_use;
    size_t blocks_free;
    size_t chunks;            /* number of mmap regions */
} balloc_stats;

void balloc_stats_get(balloc_stats *out);

/* Validate every heap invariant. Returns 0 if the heap is consistent, or a
 * negative error code identifying the first violation found. */
int  balloc_check(void);

/* Release every chunk back to the OS and reset to empty (test convenience). */
void balloc_reset(void);

#endif /* BALLOC_H */
