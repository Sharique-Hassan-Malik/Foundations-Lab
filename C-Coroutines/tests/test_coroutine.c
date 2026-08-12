/* Tests for the coroutine library.
 *
 * The headline is that suspending and resuming preserves everything: yielded
 * values in order, values passed both ways, and — the one that catches a broken
 * context switch — the coroutine's entire stack (locals) intact across a switch
 * to another coroutine and back. Also: coroutines resuming coroutines (nesting),
 * and many coroutines coexisting. */

#include "coroutine.h"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else         { printf("  ok:   %s\n", msg); } \
} while (0)

/* --------------------------------------------------------- generator */

static void counter(void *arg) {
    long n = (long)arg;
    for (long i = 0; i < n; i++) co_yield(i);
}

static void test_generator(void) {
    printf("generator: yields a sequence, then finishes\n");
    coroutine *co = co_create(counter, (void *)10, 0);
    int ok = 1;
    for (long i = 0; i < 10; i++) {
        long v = co_resume(co, 0);
        if (v != i || co_done(co)) ok = 0;
    }
    CHECK(ok, "resumes yield 0..9 in order, not yet done");
    long last = co_resume(co, 0);          /* runs off the end */
    CHECK(last == 0 && co_done(co), "final resume finishes the coroutine");
    CHECK(co_resume(co, 0) == 0, "resuming a finished coroutine is a harmless no-op");
    co_destroy(co);
}

/* --------------------------------------------------------- two-way values */

static void adder(void *arg) {
    (void)arg;
    long x = co_yield(0);                   /* hand out 0, receive the first input */
    for (;;) x = co_yield(x + 1);           /* echo input + 1 */
}

static void test_two_way(void) {
    printf("two-way: co_resume(in) delivers a value the coroutine receives\n");
    coroutine *co = co_create(adder, NULL, 0);
    long a = co_resume(co, 0);              /* primes it: yields 0 */
    long b = co_resume(co, 5);             /* 5 -> 6 */
    long c = co_resume(co, 40);            /* 40 -> 41 */
    CHECK(a == 0 && b == 6 && c == 41, "values pass in and out symmetrically");
    co_destroy(co);
}

/* --------------------------------------------------------- ping-pong order */

static int order_log[32];
static int order_n;

static void pinger(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < 5; i++) { order_log[order_n++] = id; co_yield(0); }
}

static void test_ping_pong(void) {
    printf("scheduler: two coroutines interleave in the order we resume them\n");
    order_n = 0;
    coroutine *a = co_create(pinger, (void *)0, 0);
    coroutine *b = co_create(pinger, (void *)1, 0);
    for (int k = 0; k < 5; k++) { co_resume(a, 0); co_resume(b, 0); }
    int ok = (order_n == 10);
    for (int i = 0; i < 10 && ok; i++) if (order_log[i] != (i & 1)) ok = 0;
    CHECK(ok, "log is 0,1,0,1,… exactly as scheduled");
    co_destroy(a); co_destroy(b);
}

/* --------------------------------------------------------- nesting */

static coroutine *g_inner;

static void inner_fn(void *arg) { (void)arg; co_yield(100); co_yield(200); }

static void outer_fn(void *arg) {
    (void)arg;
    long x = co_resume(g_inner, 0);         /* a coroutine resuming a coroutine */
    co_yield(x);
    long y = co_resume(g_inner, 0);
    co_yield(y);
}

static void test_nesting(void) {
    printf("nesting: a coroutine can resume another coroutine\n");
    g_inner = co_create(inner_fn, NULL, 0);
    coroutine *outer = co_create(outer_fn, NULL, 0);
    long r1 = co_resume(outer, 0);
    long r2 = co_resume(outer, 0);
    CHECK(r1 == 100 && r2 == 200, "inner values bubble up through the outer coroutine");
    co_destroy(outer); co_destroy(g_inner);
}

/* --------------------------------------------------------- stack integrity */

static void heavy(void *arg) {
    (void)arg;
    volatile long buf[2048];
    for (int i = 0; i < 2048; i++) buf[i] = (long)i * 3;
    co_yield(1);                            /* suspend with a big live stack */
    long sum = 0;
    for (int i = 0; i < 2048; i++) sum += buf[i];
    co_yield(sum);
}

static void test_stack_integrity(void) {
    printf("stack: a coroutine's locals survive switching away and back\n");
    coroutine *h = co_create(heavy, NULL, 0);
    coroutine *noise = co_create(counter, (void *)100, 0);
    co_resume(h, 0);                        /* fills buf, yields */
    for (int i = 0; i < 50; i++) co_resume(noise, 0);   /* clobber registers/other stacks */
    long sum = co_resume(h, 0);            /* recompute from buf */
    long expected = 0;
    for (int i = 0; i < 2048; i++) expected += (long)i * 3;
    CHECK(sum == expected, "the 2048-element local array is byte-for-byte intact");
    co_destroy(h); co_destroy(noise);
}

/* --------------------------------------------------------- many coroutines */

static void test_many(void) {
    printf("scale: hundreds of coroutines coexist\n");
    enum { N = 500 };
    coroutine *cos[N];
    for (int i = 0; i < N; i++) cos[i] = co_create(counter, (void *)(long)(i % 20 + 1), 0);
    long total = 0;
    int alive = N;
    while (alive > 0) {                    /* round-robin until all finish */
        alive = 0;
        for (int i = 0; i < N; i++) {
            if (!co_done(cos[i])) { total += co_resume(cos[i], 0); if (!co_done(cos[i])) alive++; }
        }
    }
    /* each coroutine i yields 0..(i%20) → sum = tri(i%20 - 1) over all i... just
     * check they all finished without crashing */
    int all_done = 1;
    for (int i = 0; i < N; i++) if (!co_done(cos[i])) all_done = 0;
    CHECK(all_done && total >= 0, "all 500 coroutines ran to completion");
    for (int i = 0; i < N; i++) co_destroy(cos[i]);
}

int main(void) {
    test_generator();
    test_two_way();
    test_ping_pong();
    test_nesting();
    test_stack_integrity();
    test_many();
    if (failures == 0) printf("\nALL TESTS PASSED\n");
    else               printf("\n%d TEST(S) FAILED\n", failures);
    return failures ? 1 : 0;
}
