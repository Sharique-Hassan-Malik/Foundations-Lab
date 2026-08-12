// workstealing — a work-stealing thread pool built on a lock-free Chase-Lev deque.
//
// Each worker owns a double-ended queue of tasks. It pushes and pops at the
// *bottom* of its own deque (LIFO, cache-friendly, and — because only the owner
// touches that end in the common case — almost free). When a worker runs dry it
// becomes a thief and *steals* from the *top* of another worker's deque. Owner
// and thieves operate on opposite ends, so they rarely contend; the one place
// they can race — the last remaining element — is resolved by a single
// compare-and-swap on the shared `top` index. No locks are held on the hot path.
//
// This is the scheduler at the heart of Cilk, Intel TBB, Rust's rayon and Go's
// runtime. The deque follows Lê et al., "Correct and Efficient Work-Stealing for
// Weak Memory Models", including its C11 memory orders.

#ifndef WORKSTEALING_HPP
#define WORKSTEALING_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace ws {

struct Task {
    std::function<void()> fn;
};

// ------------------------------------------------------------ Chase-Lev deque

class Deque {
    // A power-of-two ring of atomic task pointers. Only the owner ever grows it;
    // superseded rings are retired (kept alive until the deque dies) so a thief
    // mid-steal never dereferences freed memory.
    struct Ring {
        int64_t cap;
        int64_t mask;
        std::atomic<Task*>* slots;
        explicit Ring(int64_t c) : cap(c), mask(c - 1), slots(new std::atomic<Task*>[c]) {}
        ~Ring() { delete[] slots; }
        Task* get(int64_t i) const { return slots[i & mask].load(std::memory_order_relaxed); }
        void  put(int64_t i, Task* x) { slots[i & mask].store(x, std::memory_order_relaxed); }
    };

    std::atomic<int64_t> top_{0};
    std::atomic<int64_t> bottom_{0};
    std::atomic<Ring*>   ring_;
    std::vector<Ring*>   retired_;   // owner-only; freed in the destructor

public:
    explicit Deque(int64_t cap = 1024) : ring_(new Ring(cap)) {}
    ~Deque() {
        delete ring_.load();
        for (Ring* r : retired_) delete r;
    }
    Deque(const Deque&) = delete;
    Deque& operator=(const Deque&) = delete;

    // Owner only.
    void push(Task* x) {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);
        Ring* a = ring_.load(std::memory_order_relaxed);
        if (b - t > a->cap - 1) {                       // full → grow
            Ring* na = new Ring(a->cap * 2);
            for (int64_t i = t; i < b; i++) na->put(i, a->get(i));
            retired_.push_back(a);
            ring_.store(na, std::memory_order_relaxed);
            a = na;
        }
        a->put(b, x);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // Owner only. Returns nullptr if empty.
    Task* pop() {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        Ring* a = ring_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_relaxed);
        Task* x = nullptr;
        if (t <= b) {
            x = a->get(b);
            if (t == b) {                               // last element: race a thief
                if (!top_.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed))
                    x = nullptr;                        // a thief won it
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
        } else {                                        // already empty
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return x;
    }

    // Any thief. Returns nullptr on empty or on a lost race (caller just retries).
    Task* steal() {
        int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom_.load(std::memory_order_acquire);
        Task* x = nullptr;
        if (t < b) {
            Ring* a = ring_.load(std::memory_order_acquire);
            x = a->get(t);
            if (!top_.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed))
                return nullptr;                          // lost the race
        }
        return x;
    }

    bool empty() const {
        return bottom_.load(std::memory_order_relaxed) <=
               top_.load(std::memory_order_relaxed);
    }
};

// ------------------------------------------------------------ the thread pool

class ThreadPool {
    std::vector<std::unique_ptr<Deque>> deques_;
    std::vector<std::thread> workers_;
    std::atomic<bool> done_{false};
    std::atomic<int64_t> pending_{0};        // tasks submitted but not yet finished
    std::atomic<uint64_t> steals_{0};
    std::mutex idle_mtx_;
    std::condition_variable idle_cv_;
    std::mutex gq_mtx_;                       // global submission queue: the only
    std::deque<Task*> gq_;                    // safe entry point for outside threads
    static thread_local int worker_id_;

public:
    explicit ThreadPool(unsigned n = std::thread::hardware_concurrency()) {
        if (n == 0) n = 1;
        for (unsigned i = 0; i < n; i++) deques_.push_back(std::make_unique<Deque>());
        for (unsigned i = 0; i < n; i++)
            workers_.emplace_back([this, i] { run(i); });
    }

    ~ThreadPool() {
        done_.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(idle_mtx_); }
        idle_cv_.notify_all();
        for (auto& w : workers_) if (w.joinable()) w.join();
    }

    size_t size() const { return workers_.size(); }
    uint64_t steal_count() const { return steals_.load(); }

    // Block the calling (non-worker) thread until every submitted task, and any
    // tasks they spawned, have finished. This is the deadlock-free way to do
    // fork-join: workers never block on a future, they just keep running tasks,
    // and the outside thread waits on the pending counter.
    void wait_idle() {
        std::unique_lock<std::mutex> lk(idle_mtx_);
        idle_cv_.wait(lk, [this] { return pending_.load(std::memory_order_relaxed) == 0; });
    }

    // Submit a callable; returns a future for its result. Callable-into-pool
    // (fork-join) is supported: a running task may submit more.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        auto pt = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> fut = pt->get_future();
        Task* t = new Task{[pt] { (*pt)(); }};
        enqueue(t);
        return fut;
    }

private:
    void enqueue(Task* t) {
        pending_.fetch_add(1, std::memory_order_relaxed);
        // A Chase-Lev deque may only be pushed by its owning worker. So a worker
        // spawning a task (fork-join) pushes to its own deque — the fast path —
        // while any outside thread hands the task to the shared global queue,
        // which workers drain. This keeps the single-owner invariant intact.
        if (worker_id_ >= 0) {
            deques_[worker_id_]->push(t);
        } else {
            std::lock_guard<std::mutex> lk(gq_mtx_);
            gq_.push_back(t);
        }
        { std::lock_guard<std::mutex> lk(idle_mtx_); }
        idle_cv_.notify_one();
    }

    Task* take_global() {
        std::lock_guard<std::mutex> lk(gq_mtx_);
        if (gq_.empty()) return nullptr;
        Task* t = gq_.front();
        gq_.pop_front();
        return t;
    }

    Task* steal_from_others(int self, std::mt19937& rng) {
        int n = (int)deques_.size();
        if (n <= 1) return nullptr;
        std::uniform_int_distribution<int> pick(0, n - 1);
        for (int tries = 0; tries < n * 2; tries++) {
            int v = pick(rng);
            if (v == self) continue;
            if (Task* t = deques_[v]->steal()) {
                steals_.fetch_add(1, std::memory_order_relaxed);
                return t;
            }
        }
        return nullptr;
    }

    void run(int id) {
        worker_id_ = id;
        std::mt19937 rng(1234 + id);
        while (!done_.load(std::memory_order_acquire)) {
            Task* t = deques_[id]->pop();          // own work first (fork-join, LIFO)
            if (!t) t = take_global();             // then externally submitted work
            if (!t) t = steal_from_others(id, rng); // then steal from a busy worker
            if (t) {
                t->fn();
                delete t;
                if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lk(idle_mtx_);   // last task done
                    idle_cv_.notify_all();
                }
                continue;
            }
            // nothing to do: sleep until work arrives or shutdown
            std::unique_lock<std::mutex> lk(idle_mtx_);
            idle_cv_.wait_for(lk, std::chrono::milliseconds(1), [this] {
                return done_.load(std::memory_order_acquire) ||
                       pending_.load(std::memory_order_relaxed) > 0;
            });
        }
    }
};

inline thread_local int ThreadPool::worker_id_ = -1;

}  // namespace ws

#endif  // WORKSTEALING_HPP
