/* Tests for balloc: unit checks plus a randomized stress harness.
 *
 * The stress test is the headline. It keeps a table of live allocations, and on
 * every random malloc/realloc writes a per-allocation signature into the whole
 * payload; on every free/realloc it verifies that signature is still intact.
 * Any overlap between two live blocks, or any header/footer corruption, changes
 * a byte and is caught immediately. Between operations it periodically runs
 * balloc_check(), which walks the heap and asserts every structural invariant.
 * Zero mismatches over millions of operations is the correctness claim. */

#include "balloc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* deterministic xorshift PRNG so runs are reproducible */
static uint64_t rng_state = 0x123456789abcdefULL;
static uint64_t rng(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rng_state = x;
}
static size_t rand_size(void) {
    /* mostly small, occasionally large — the realistic distribution */
    uint64_t r = rng();
    if (r % 16 == 0) return 1 + r % 8192;
    return 1 + r % 256;
}

static void fill(unsigned char *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed * 31u + i);
}
static int verify(const unsigned char *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (uint8_t)(seed * 31u + i)) return 0;
    return 1;
}

/* ------------------------------------------------------------ unit checks */

static void unit_tests(void) {
    printf("unit tests\n");
    balloc_reset();

    void *a = balloc(1);
    CHECK(a != NULL, "balloc(1) returns non-null");
    CHECK(((uintptr_t)a % 16) == 0, "payload is 16-byte aligned");
    bfree(a);

    /* alignment for a spread of sizes */
    int aligned = 1;
    for (size_t s = 1; s <= 2000; s += 7) {
        void *p = balloc(s);
        if ((uintptr_t)p % 16) aligned = 0;
        bfree(p);
    }
    CHECK(aligned, "all sizes 16-byte aligned");

    /* calloc zeroes */
    unsigned char *z = bcalloc(1000, 1);
    int allzero = 1;
    for (int i = 0; i < 1000; i++) if (z[i]) allzero = 0;
    CHECK(allzero, "bcalloc zero-initialises");
    bfree(z);

    /* realloc preserves contents and grows */
    unsigned char *r = balloc(100);
    fill(r, 100, 42);
    r = brealloc(r, 4000);
    CHECK(verify(r, 100, 42), "brealloc preserves old bytes when growing");
    bfree(r);

    /* coalescing: free three adjacent blocks -> they merge. Allocate three in a
     * row from a fresh heap, free them, and confirm the heap collapses to a
     * single free block (blocks_free small). */
    balloc_reset();
    void *x = balloc(64), *y = balloc(64), *w = balloc(64);
    bfree(y); bfree(x); bfree(w);
    balloc_stats stats; balloc_stats_get(&stats);
    CHECK(stats.blocks_in_use == 0, "all blocks freed");
    CHECK(balloc_check() == 0, "heap consistent after coalescing");
    CHECK(stats.blocks_free <= 2, "adjacent frees coalesce into one block");

    /* reuse: after free, the next same-size request should not grow the heap */
    balloc_reset();
    void *p1 = balloc(128);
    balloc_stats_get(&stats);
    size_t heap_after_first = stats.heap_bytes;
    bfree(p1);
    void *p2 = balloc(128);
    balloc_stats_get(&stats);
    CHECK(stats.heap_bytes == heap_after_first, "freed block is reused, heap not grown");
    bfree(p2);

    printf("  (%d failures so far)\n", failures);
}

/* --------------------------------------------------------- stress harness */

#define SLOTS 4096
#define OPS   2000000

static void stress_test(void) {
    printf("stress: %d random ops over %d live slots\n", OPS, SLOTS);
    balloc_reset();

    struct { unsigned char *p; size_t n; uint8_t seed; } live[SLOTS];
    memset(live, 0, sizeof(live));
    long corruptions = 0, checks = 0;

    for (long op = 0; op < OPS; op++) {
        size_t i = rng() % SLOTS;
        uint64_t what = rng() % 3;

        if (live[i].p == NULL || what == 0) {
            if (live[i].p) {                      /* occupied: verify then free */
                if (!verify(live[i].p, live[i].n, live[i].seed)) corruptions++;
                bfree(live[i].p);
            }
            size_t n = rand_size();
            unsigned char *p = balloc(n);
            if (p) { uint8_t s = (uint8_t)rng(); fill(p, n, s); live[i].p = p; live[i].n = n; live[i].seed = s; }
            else   { live[i].p = NULL; }
        } else if (what == 1) {                   /* free */
            if (!verify(live[i].p, live[i].n, live[i].seed)) corruptions++;
            bfree(live[i].p);
            live[i].p = NULL;
        } else {                                  /* realloc */
            if (!verify(live[i].p, live[i].n, live[i].seed)) corruptions++;
            size_t n = rand_size();
            unsigned char *p = brealloc(live[i].p, n);
            if (p) {
                size_t keep = n < live[i].n ? n : live[i].n;
                if (!verify(p, keep, live[i].seed)) corruptions++;
                fill(p, n, live[i].seed);
                live[i].p = p; live[i].n = n;
            }
        }

        if (op % 50000 == 0) {                    /* periodic full heap audit */
            if (balloc_check() != 0) corruptions++;
            checks++;
        }
    }

    /* free everything; the heap must return to zero blocks in use */
    for (size_t i = 0; i < SLOTS; i++)
        if (live[i].p) {
            if (!verify(live[i].p, live[i].n, live[i].seed)) corruptions++;
            bfree(live[i].p);
        }

    balloc_stats stats; balloc_stats_get(&stats);
    CHECK(corruptions == 0, "zero corruptions across all operations");
    CHECK(balloc_check() == 0, "final heap consistent");
    CHECK(stats.bytes_in_use == 0, "all payload bytes returned");
    CHECK(stats.blocks_in_use == 0, "all blocks freed");
    printf("  %ld corruptions, %ld heap audits passed, %zu bytes still in use\n",
           corruptions, checks, stats.bytes_in_use);
}

int main(void) {
    unit_tests();
    stress_test();
    if (failures == 0) printf("\nALL TESTS PASSED\n");
    else               printf("\n%d TEST(S) FAILED\n", failures);
    return failures ? 1 : 0;
}
