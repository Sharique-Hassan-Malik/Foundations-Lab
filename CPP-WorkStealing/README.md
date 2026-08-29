# C++ Work-Stealing Scheduler

A **work-stealing thread pool** in C++17, built on a lock-free **Chase–Lev deque** — the scheduler design at the heart of Cilk, Intel TBB, Rust's rayon, and the Go runtime. Each worker owns a double-ended queue: it pushes and pops its own tasks at one end (cheap, cache-friendly, contention-free in the common case), and when it runs dry it *steals* from the other end of a busy worker's queue. No locks are held on the hot path.

## The headline: exactly-once under contention

A scheduler is only correct if every task runs **exactly once** — never dropped, never run twice — no matter how a worker's pops race against other workers' steals. That is the property under test, and it is verified directly: one owner and three thieves pound a single deque with 200,000 tasks while the harness counts how many times each task is dequeued.

```
$ make test

deque: one owner + thieves, every task dequeued exactly once
  ok:   every task ran (sum of counts == N)
  ok:   no task ran twice
  ok:   no task was lost
pool: every submitted task runs exactly once
  ok:   counter equals number of submitted tasks
pool: fork-join (tasks spawn tasks), no deadlock
  ok:   recursive parallel sum equals n(n-1)/2
pool: work actually gets stolen on an unbalanced load
  ok:   the thieves stole work (steal_count > 0)
  (steals: 19908 of 20000 tasks)

ALL TESTS PASSED
```

The whole suite is also clean under **AddressSanitizer**, which is what enforces the deque's single-owner rule: an outside thread pushing directly onto a worker's deque lets a task be taken twice. External submissions go through a small global queue, and only a worker ever pushes to its own deque.

## Scaling

Work-stealing's strength is **fork-join**: a task recursively splits itself, spawning children onto its own deque, and idle workers steal the surplus — no shared queue in the path, so it scales with cores.

```
$ make bench     # divide-and-conquer over 20000 deliberately uneven leaves

   1 worker : 4.376 s   (1.00x)
   2 workers: 2.252 s   (1.94x speedup)
   4 workers: 1.958 s   (2.23x speedup)
```

The test machine is a 2-**physical**-core i5 with hyperthreading (4 logical CPUs), so **1.94× on 2 workers is near-perfect linear scaling on the real cores**; the 4-worker figure is the extra hyperthreads sharing execution units. On a box with more physical cores the curve continues — the scheduler keeps every worker busy despite a 10× spread in per-task cost, which a naive static split could not.

## Using it

```cpp
#include "workstealing.hpp"

ws::ThreadPool pool;                                  // one worker per hardware thread

auto fut = pool.submit([](int x){ return x * x; }, 7);
int r = fut.get();                                    // 49

// fork-join: a task may submit more tasks; wait_idle() blocks (deadlock-free)
// until everything, including spawned work, has finished
std::atomic<long> sum{0};
std::function<void(int,int)> divide = [&](int lo, int hi){
    if (hi - lo <= 64) { for (int i=lo;i<hi;i++) sum += i; return; }
    int mid = (lo+hi)/2;
    pool.submit([&,lo,mid]{ divide(lo,mid); });
    pool.submit([&,mid,hi]{ divide(mid,hi); });
};
pool.submit([&]{ divide(0, 1000000); });
pool.wait_idle();
```

## Build & run

```bash
make test      # unit + concurrent exactly-once + fork-join + stealing
make bench      # fork-join scaling across worker counts
make clean

# the suite is sanitiser-clean:
g++ -O1 -g -std=c++17 -pthread -Iinclude -fsanitize=address tests/test_workstealing.cpp -o t && ./t
```

Header-only; needs a C++17 compiler and pthreads (tested with g++ 13).

## Layout

| path | what it holds |
|---|---|
| `include/workstealing.hpp` | the Chase–Lev `Deque` and the `ThreadPool` (header-only) |
| `tests/test_workstealing.cpp` | LIFO, the concurrent exactly-once proof, futures, fork-join, stealing |
| `bench/bench.cpp` | fork-join scaling benchmark |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the deque's push/pop/steal protocol, the single memory-ordering subtlety that makes it correct, and why external submission needs a separate path.

## License

MIT — see [`LICENSE`](./LICENSE).
