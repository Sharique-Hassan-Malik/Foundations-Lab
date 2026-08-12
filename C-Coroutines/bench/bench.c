/* Measure context-switch throughput: a coroutine that does nothing but yield in
 * a tight loop, driven by a resume in a tight loop. Each iteration is two
 * switches (into the coroutine and back out). */

#define _POSIX_C_SOURCE 199309L

#include "coroutine.h"

#include <stdio.h>
#include <time.h>

static void spinner(void *arg) {
    long n = (long)arg;
    for (long i = 0; i < n; i++) co_yield(i);
}

int main(void) {
    const long N = 50000000;
    coroutine *co = co_create(spinner, (void *)N, 0);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < N; i++) co_resume(co, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    long switches = 2 * N;                 /* resume + yield per iteration */
    printf("  %ld context switches in %.3f s\n", switches, sec);
    printf("  = %.1f million switches/second  (%.1f ns each)\n",
           switches / sec / 1e6, sec / switches * 1e9);
    co_destroy(co);
    return 0;
}
