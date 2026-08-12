// Scaling benchmark. Work-stealing shines on *fork-join*: a task recursively
// splits itself, spawning children onto its own local deque, and idle workers
// steal the surplus. That path avoids any shared queue, so it scales with cores.
// We run a deliberately uneven divide-and-conquer compute at 1, 2, 4 … workers
// and report the speedup.

#include "workstealing.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>

// uneven leaf cost: work at index i varies ~10x, so a static split would stall
static double heavy(int i) {
    double acc = 0;
    int iters = 2000 + (i * 37) % 20000;
    for (int k = 0; k < iters; k++) acc += std::sin(i * 0.001 + k);
    return acc;
}

static void atomic_add(std::atomic<double>& a, double v) {
    double c = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(c, c + v, std::memory_order_relaxed)) {}
}

static double run_with(unsigned nworkers, int n) {
    ws::ThreadPool pool(nworkers);
    std::atomic<double> total{0.0};
    std::function<void(int, int)> divide = [&](int lo, int hi) {
        if (hi - lo <= 32) {
            double s = 0;
            for (int i = lo; i < hi; i++) s += heavy(i);
            atomic_add(total, s);
            return;
        }
        int mid = (lo + hi) / 2;
        pool.submit([&divide, lo, mid] { divide(lo, mid); });
        pool.submit([&divide, mid, hi] { divide(mid, hi); });
    };
    auto t0 = std::chrono::steady_clock::now();
    pool.submit([&divide, n] { divide(0, n); });
    pool.wait_idle();
    auto t1 = std::chrono::steady_clock::now();
    (void)total.load();
    return std::chrono::duration<double>(t1 - t0).count();
}

int main() {
    const int n = 20000;
    unsigned hw = std::thread::hardware_concurrency();
    std::printf("fork-join workload: divide-and-conquer over %d uneven leaves, "
                "hardware_concurrency = %u\n\n", n, hw);

    double base = run_with(1, n);
    std::printf("  %2u worker : %.3f s   (1.00x)\n", 1u, base);
    for (unsigned k = 2; k <= hw; k *= 2) {
        double t = run_with(k, n);
        std::printf("  %2u workers: %.3f s   (%.2fx speedup)\n", k, t, base / t);
    }
    return 0;
}
