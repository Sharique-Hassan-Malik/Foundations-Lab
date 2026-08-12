# Architecture

Two pieces: the lock-free **Chase–Lev deque** (one per worker) and the **thread pool** that schedules across them. The design goal is that the common operations — a worker pushing and popping its own work — touch no shared state and take no locks, while the rare cross-worker interaction (a steal, or the last element) is resolved by a single atomic compare-and-swap.

## The Chase–Lev deque

A deque is a power-of-two ring of task pointers plus two 64-bit indices: `top` (the steal end) and `bottom` (the owner end). The live tasks are the slots in `[top, bottom)`, indexed modulo the ring size. Three operations, with strict rules about who may call them:

- **`push` (owner only)** — store the task at `bottom`, then publish it by advancing `bottom`. A release fence between the store and the `bottom` bump ensures a thief that sees the new `bottom` also sees the task.
- **`pop` (owner only)** — decrement `bottom` first, then a **sequentially-consistent fence**, then read `top`. That fence is the crux of the whole algorithm: it orders the owner's `bottom` write against a thief's `top` write so they cannot both claim the last element. If `top < bottom` after the decrement there is more than one task and the owner simply takes the bottom one. If `top == bottom` there is exactly one task and owner and thief are racing for it — the owner tries to win it with a CAS `top: t → t+1`; whoever's CAS succeeds gets it, the loser backs off. If `top > bottom` the deque was empty; restore `bottom`.
- **`steal` (any thief)** — read `top`, a seq-cst fence, then `bottom`; if `top < bottom`, read the task at `top` and try to claim it with a CAS `top: t → t+1`. Two thieves reading the same `top` both attempt the CAS; exactly one succeeds, so a task is never stolen twice.

The invariant that falls out: every task in `[top, bottom)` is claimed by exactly one successful `top`-advance or one owner `pop`, so it is dequeued **exactly once**. The seq-cst fences in `pop` and `steal` are what make this hold on weakly-ordered hardware (the implementation follows Lê et al.'s memory orders); on x86's stronger model they collapse to compiler barriers, but the code stays correct on ARM too.

**Growing.** When `push` finds the ring full it allocates a ring of double the size, copies the live range across, and switches the `ring` pointer. A thief may still be reading through the *old* ring pointer it loaded a moment earlier, so old rings are not freed immediately — they are retired to a list and released only when the deque is destroyed. That trades a little memory for freedom from the use-after-free that eager reclamation would risk.

## Why steal reads before it CASes

A thief reads `array[top]` *before* the CAS that claims it. That ordering matters: after a successful CAS, `top` has moved past the slot, so the owner will never overwrite it — the value the thief read is safely its own. If the CAS fails (another thief or the owner got there first), the thief simply discards the read and moves on. Reading trivially-copyable pointers (not the task objects themselves) keeps that speculative read tear-free.

## The pool, and the single-owner rule

Each worker runs a loop: pop its own deque (its own and its spawned work, LIFO — good locality), else take from the global queue, else steal from a randomly chosen victim, else sleep on a condition variable until work arrives or the pool shuts down. Tasks are `std::function<void()>` wrapped in a heap `Task`; `submit` wraps a callable in a `std::packaged_task` and returns its `std::future`.

The one rule the deque imposes — **only the owning worker may `push` or `pop`** — is exactly what a thread pool's API tempts you to break, because tasks are submitted from arbitrary threads. Pushing an externally-submitted task straight onto a worker's deque means two threads (the submitter and the worker) touch the owner end concurrently, and the algorithm's guarantees evaporate; in practice a task gets run twice (AddressSanitizer caught this as a use-after-free during development). So submission is split:

- a **worker** spawning a task (fork-join) pushes to *its own* deque — the fast, lock-free path that also feeds the stealing that makes fork-join scale;
- any **outside thread** hands the task to a small mutex-protected **global queue**, which every worker drains.

External bulk submission therefore serialises on one mutex — fine, because it is not the hot path and real fork-join work never goes through it. `wait_idle` lets an outside thread block until a running pending-counter hits zero, which is the deadlock-free way to join a fork-join computation: workers never block on a future (which could deadlock if every worker were waiting on work only another blocked worker could run), they just keep executing tasks, and the outside thread waits on the count.
