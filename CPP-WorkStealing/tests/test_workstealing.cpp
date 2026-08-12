// Tests for the work-stealing deque and pool.
//
// The headline test hammers a single Chase-Lev deque with one owner and several
// thieves and proves the property the whole scheduler depends on: every task is
// dequeued *exactly once* — never lost, never run twice — no matter how the
// owner's pops and the thieves' steals interleave. On top of that: the pool runs
// every submitted task once, returns correct futures, and handles fork-join
// (tasks that spawn tasks) without deadlock.

#include "workstealing.hpp"

#include <atomic>
#include <cstdio>
#include <numeric>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); failures++; } \
    else         { std::printf("  ok:   %s\n", msg); } \
} while (0)

// --------------------------------------------------------- single-thread deque

static void test_deque_lifo() {
    std::printf("deque: single-thread LIFO\n");
    ws::Deque d;
    const int N = 1000;
    for (int i = 0; i < N; i++) d.push(new ws::Task{[]{}});
    int count = 0;
    while (ws::Task* t = d.pop()) { delete t; count++; }
    CHECK(count == N, "pop returns exactly what was pushed");
    CHECK(d.pop() == nullptr, "empty deque pops null");
}

// ------------------------------------------------- concurrent deque: exactly once

static void test_deque_exactly_once() {
    std::printf("deque: one owner + thieves, every task dequeued exactly once\n");
    const int N = 200000;
    const int THIEVES = 3;
    ws::Deque d;
    std::vector<std::atomic<int>> seen(N);
    for (auto& s : seen) s.store(0);
    std::atomic<int> retrieved{0};
    std::atomic<bool> pushing_done{false};

    auto process = [&](ws::Task* t) { t->fn(); delete t; retrieved.fetch_add(1); };

    std::vector<std::thread> thieves;
    for (int k = 0; k < THIEVES; k++)
        thieves.emplace_back([&] {
            while (retrieved.load() < N) {
                if (ws::Task* t = d.steal()) process(t);
                else std::this_thread::yield();
            }
        });

    // owner: interleave pushes and pops
    for (int i = 0; i < N; i++) {
        int id = i;
        d.push(new ws::Task{[&seen, id] { seen[id].fetch_add(1); }});
        if ((i & 7) == 0) if (ws::Task* t = d.pop()) process(t);
    }
    pushing_done.store(true);
    while (retrieved.load() < N) if (ws::Task* t = d.pop()) process(t);
    for (auto& th : thieves) th.join();

    int total = 0, dup = 0, missing = 0;
    for (auto& s : seen) { int v = s.load(); total += v; if (v > 1) dup++; if (v == 0) missing++; }
    CHECK(total == N, "every task ran (sum of counts == N)");
    CHECK(dup == 0, "no task ran twice");
    CHECK(missing == 0, "no task was lost");
}

// --------------------------------------------------------------- pool: basics

static void test_pool_runs_everything_once() {
    std::printf("pool: every submitted task runs exactly once\n");
    ws::ThreadPool pool;
    const int N = 100000;
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < N; i++)
        futs.push_back(pool.submit([&counter] { counter.fetch_add(1); }));
    for (auto& f : futs) f.get();
    CHECK(counter.load() == N, "counter equals number of submitted tasks");
}

static void test_pool_futures() {
    std::printf("pool: futures carry results back\n");
    ws::ThreadPool pool;
    std::vector<std::future<int>> futs;
    for (int i = 0; i < 1000; i++)
        futs.push_back(pool.submit([](int x) { return x * x; }, i));
    bool ok = true;
    for (int i = 0; i < 1000; i++) if (futs[i].get() != i * i) ok = false;
    CHECK(ok, "each future returns f(i) = i*i");
}

// --------------------------------------------------------- pool: fork-join

static void test_pool_fork_join() {
    std::printf("pool: fork-join (tasks spawn tasks), no deadlock\n");
    ws::ThreadPool pool;
    std::atomic<long> sum{0};
    // a recursive task that splits a range and spawns children for the halves;
    // leaves add their value. No task blocks on a child — completion is detected
    // by wait_idle() on the pending counter.
    std::function<void(int, int)> divide = [&](int lo, int hi) {
        if (hi - lo <= 64) {
            long s = 0;
            for (int i = lo; i < hi; i++) s += i;
            sum.fetch_add(s);
            return;
        }
        int mid = (lo + hi) / 2;
        pool.submit([&divide, lo, mid] { divide(lo, mid); });
        pool.submit([&divide, mid, hi] { divide(mid, hi); });
    };
    const int N = 1000000;
    pool.submit([&divide, N] { divide(0, N); });
    pool.wait_idle();
    long expected = (long)N * (N - 1) / 2;
    CHECK(sum.load() == expected, "recursive parallel sum equals n(n-1)/2");
}

// ------------------------------------------------------- stealing really happens

static void test_stealing_occurs() {
    std::printf("pool: work actually gets stolen on an unbalanced load\n");
    ws::ThreadPool pool(4);
    std::atomic<int> done{0};
    // A single root task, run on one worker, spawns 20000 children into *that*
    // worker's local deque. The pile-up is exactly the imbalance work-stealing
    // exists for: the other three workers must steal from it to make progress.
    pool.submit([&pool, &done] {
        for (int i = 0; i < 20000; i++)
            pool.submit([&done] {
                volatile int x = 0; for (int j = 0; j < 200; j++) x += j;
                done.fetch_add(1);
            });
    });
    pool.wait_idle();
    CHECK(done.load() == 20000, "all spawned tasks completed");
    CHECK(pool.steal_count() > 0, "the thieves stole work (steal_count > 0)");
    std::printf("  (steals: %llu of 20000 tasks)\n", (unsigned long long)pool.steal_count());
}

int main() {
    test_deque_lifo();
    test_deque_exactly_once();
    test_pool_runs_everything_once();
    test_pool_futures();
    test_pool_fork_join();
    test_stealing_occurs();
    if (failures == 0) std::printf("\nALL TESTS PASSED\n");
    else               std::printf("\n%d CHECK(S) FAILED\n", failures);
    return failures ? 1 : 0;
}
