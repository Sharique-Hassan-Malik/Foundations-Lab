# Architecture

## The three colours

Every node is `CLEAN`, `CHECK`, or `DIRTY`, and the whole algorithm is what those
mean and how a node moves between them.

| colour | meaning |
|---|---|
| `CLEAN` | the value is current; nothing to do |
| `CHECK` | a *transitive* source changed — this node **might** be stale |
| `DIRTY` | a *direct* source changed — this node **is** stale |

The distinction between `CHECK` and `DIRTY` is the entire trick. A write knows
exactly which nodes read it directly (they become `DIRTY`) but cannot know
whether nodes further downstream are truly affected — a change to a source does
not always change a derived value. So those become `CHECK`: a question to be
answered later, cheaply, by looking rather than by recomputing.

## Two phases

**Push (on write).** `markStale` walks *up* the observer graph setting states,
and recomputes nothing:

```
set() → direct observers become DIRTY
      → their observers become CHECK
      → their observers become CHECK … stopping wherever a node is already
        at least this stale
```

The early stop — "if `state >= incoming`, return" — is what keeps a wide diamond
from being quadratic. A node reached by many paths is marked once; the second
path finds it already marked and halts. Push is `O(affected nodes)`, never
`O(paths)`.

**Pull (on read, or when an effect is flushed).** `updateIfNecessary` brings a
node up to date doing the least work possible:

```
if CHECK:
    for each source: source.updateIfNecessary()
                     if I became DIRTY, stop checking the rest
if DIRTY: recompute
become CLEAN
```

A `CHECK` node asks each source to update itself first. If some source's value
actually changed, `update()` (below) will have flipped this node to `DIRTY`, and
we can stop checking the remaining sources and recompute. If every source turns
out unchanged, the node was a false alarm and goes straight to `CLEAN` without
recomputing. This is the equality cutoff and the glitch-freedom in one loop.

## Why the diamond is glitch-free

Trace `a.set(10)` on `a → b, c → d`, with an effect reading `d`:

1. **Push.** `a`'s observers `b`, `c` become `DIRTY`. Their observer `d` becomes
   `CHECK`. `d`'s observer (the effect) becomes `CHECK` and is queued.
2. **Flush** pulls the effect, which reads `d`.
3. `d` is `CHECK`, so it checks its sources. It updates `b` (DIRTY → recompute →
   value changed → `d` flipped to DIRTY), then updates `c` (DIRTY → recompute).
   Both are now current.
4. `d` is `DIRTY`, so it recomputes — **once**, reading a consistent `b` and `c`.

`d` is never computed with a fresh `b` and a stale `c`, because it is not
computed until *it* is pulled, and pulling it updates both sources first. The
intermediate state the naive system exposes simply never becomes visible.

## Recomputation re-tracks dependencies

`update()` clears a node's sources before running its function, and `get()`
re-adds whatever is actually read this time:

```
update():
    detach from all current sources        // they might not be read this run
    currentReaction = this
    value' = fn()                           // get() re-attaches sources here
    if value' !== value (Object.is):
        value = value'
        mark every observer DIRTY           // only a real change propagates
```

Two consequences:

- **Dynamic dependencies.** `computed(() => cond() ? x() : y())` depends on `x`
  or `y`, never both, and switches automatically when `cond` flips. Changing the
  branch not taken cannot invalidate it, because it is not a source.
- **The equality cutoff propagates.** Observers are marked `DIRTY` *only* when
  the recomputed value differs. An unchanged derived value ends the cascade,
  which is why a chain of booleans over a churning number stays quiet.

## Effects and scheduling

An effect is a reaction with no reader to pull it, so it schedules itself. When a
write marks an effect stale, the effect is pushed onto `pendingEffects`; `flush`
drains that queue after the write settles, and each effect pulls its own value —
which lazily updates exactly the computeds feeding it and no others.

`batch` raises a depth counter that makes `flush` defer until the outermost batch
closes, so N writes produce one effect run instead of N. A `Set`-free queue with
a `wasClean` guard ensures an effect invalidated by several signals in one batch
is enqueued once.

## Cleanup and disposal

An effect's function may return a cleanup, run before its next execution and on
dispose — the hook for subscriptions, timers and listeners. Cleanups run under
`untrack`, so a value read while tearing down does not accidentally create a new
dependency. `dispose()` runs the final cleanup and detaches the effect from every
source, after which it observes nothing.

## Complexity

| operation | cost |
|---|---|
| read a clean value | `O(1)` |
| write a signal (push) | `O(nodes transitively marked)` |
| settle (pull) | `O(nodes actually stale)` |
| a chain of D diamonds | `O(D)` — never the `O(2^D)` paths |

The naive foil in `bench/naive.js` is the same graph with none of this: eager
push, no memo, no dedup. Its cost tracks paths, so the benchmark's `2^D` column
and its `naive evals` column are the same shape.

## Files

| file | role |
|---|---|
| `src/signals.js` | the whole library: `Reactive`, the three-colour scheme, the API |
| `bench/naive.js` | the eager-push foil, exposing the same API |
| `bench/diamond.js` | the chain-of-diamonds scaling table and the glitch demo |
| `test/signals.test.js` | 17 tests, `node --test` |

## What is deliberately absent

- **Async reactivity / resources.** Effects are synchronous. Async is a real,
  separate design (Suspense, `createResource`) and not a small addition.
- **Intrusive linked lists.** Production libraries avoid `Set` allocation with
  hand-rolled linked lists indexed by slot. That lowers the constant factor and
  raises the line count several-fold, changing nothing about correctness or
  complexity class — so this keeps the `Set`s and the clarity.
- **Ownership / context trees.** SolidJS nests effects under owners for
  automatic disposal. Here disposal is explicit via the returned disposer.
