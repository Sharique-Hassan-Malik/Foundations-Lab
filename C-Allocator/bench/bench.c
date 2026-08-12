/* Benchmark balloc against the system malloc on the same random workload, and
 * report throughput plus a fragmentation figure (payload in use / heap taken). */

#define _POSIX_C_SOURCE 199309L   /* clock_gettime / CLOCK_MONOTONIC */

#include "balloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t rng_state = 0xC0FFEEULL;
static uint64_t rng(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rng_state = x;
}
static size_t rand_size(void) {
    uint64_t r = rng();
    return (r % 16 == 0) ? 1 + r % 8192 : 1 + r % 256;
}

#define SLOTS 4096
#define OPS   3000000

static double run(int use_system) {
    void *live[SLOTS];
    for (int i = 0; i < SLOTS; i++) live[i] = NULL;
    rng_state = 0xC0FFEEULL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long op = 0; op < OPS; op++) {
        size_t i = rng() % SLOTS;
        if (live[i]) { use_system ? free(live[i]) : bfree(live[i]); live[i] = NULL; }
        else         { size_t n = rand_size(); live[i] = use_system ? malloc(n) : balloc(n); }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < SLOTS; i++) if (live[i]) (use_system ? free : bfree)(live[i]);

    return (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
}

int main(void) {
    printf("workload: %d random malloc/free ops, %d live slots\n\n", OPS, SLOTS);

    balloc_reset();
    double tb = run(0);
    /* measure fragmentation at a steady-state fill */
    void *live[SLOTS];
    rng_state = 1;
    for (int i = 0; i < SLOTS; i++) live[i] = balloc(rand_size());
    balloc_stats s; balloc_stats_get(&s);
    for (int i = 0; i < SLOTS; i++) bfree(live[i]);

    double ts = run(1);

    printf("  balloc:        %.3f s  (%.1f Mops/s)\n", tb, OPS / tb / 1e6);
    printf("  system malloc: %.3f s  (%.1f Mops/s)\n", ts, OPS / ts / 1e6);
    printf("  balloc / system ratio: %.2fx\n\n", tb / ts);
    printf("  at %d live blocks: %.2f MiB payload in %.2f MiB heap "
           "(%.1f%% utilisation)\n",
           SLOTS, s.bytes_in_use / 1048576.0, s.heap_bytes / 1048576.0,
           100.0 * s.bytes_in_use / (double)s.heap_bytes);
    return 0;
}
