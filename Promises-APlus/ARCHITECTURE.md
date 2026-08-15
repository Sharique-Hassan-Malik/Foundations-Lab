# Architecture

## The shape

```
   new APromise(executor)
        │  executor(resolve, reject)   — may throw, which rejects
        ▼
   #resolve(value) ──▶ Promise Resolution Procedure §2.3 ──▶ #fulfil | #reject
        │                                                          │
   pending ──────────────────────────────────────────────────▶ settled
        │                                                          │
   then(onF, onR) queues a handler                    #flush schedules queued
   while pending; schedules it once settled            handlers on microtasks
        │
        ▼
   a new promise `next`, whose state comes from the handler's outcome
```

One class, one free function. The class owns state and the callback queue; the
free function `resolvePromise` is the §2.3 procedure, kept separate because it
operates on fulfil/reject *capabilities* and must recurse.

## State, and the settle-once guard

Three states — `PENDING`, `FULFILLED`, `REJECTED` — and a promise leaves
`PENDING` exactly once. Every transition begins with `if (#state !== PENDING)
return`. This is not defensive padding; it is a correctness requirement. A
thenable handed to the resolution procedure is untrusted code that the spec
explicitly allows to misbehave — call both callbacks, call one twice, throw
after resolving — and the guard is what makes all of those safe.

## then() and the second promise

`then` returns a **new** promise, `next`, and registers a handler that routes the
current promise's outcome into it (§2.2.7):

- current fulfils, `onFulfilled` returns `v` → `next` resolves with `v`
  (adopting `v`'s state if it is a thenable)
- current fulfils, `onFulfilled` throws → `next` rejects with the throw
- a **non-function** handler is ignored and the value/reason passes straight
  through to `next` — this is what makes `.then(onlyOnFulfilled)` still propagate
  a rejection down the chain

While the promise is pending, handlers are queued; once it settles, `#flush`
schedules every queued handler, and any handler registered afterward is scheduled
immediately. Either way, `#schedule` wraps the call in `queueMicrotask`, so §2.2.4
(handlers run after `then` returns) holds on every path.

## The resolution procedure, clause by clause

`resolvePromise(promise, x, fulfil, reject)` implements §2.3:

| clause | case | action |
|---|---|---|
| 2.3.1 | `x === promise` | reject with `TypeError` — a cycle |
| 2.3.3.1 | reading `x.then` throws | reject with the thrown value |
| 2.3.3.3 | `x.then` is a function | call it; adopt its first callback |
| 2.3.3.3.1 | it resolves with `y` | recurse: `resolvePromise(promise, y, …)` |
| 2.3.3.3.2 | it rejects with `r` | reject with `r` |
| 2.3.3.3.3 | it calls back more than once | honour only the first (`called` flag) |
| 2.3.3.3.4 | it throws after a callback | ignore the throw; the callback won |
| 2.3.3.4 | `x.then` is a value, not a function | fulfil with `x` |
| 2.3.4 | `x` is a primitive | fulfil with `x` |

Three of these are where naive implementations fail:

- **The cycle (2.3.1).** `let p = ...; resolve(p)` must reject, not hang. A one-
  line identity check, easy to forget.
- **The `called` guard (2.3.3.3.3).** A thenable's `then` is arbitrary code. It
  may resolve, then reject, then throw. Only the first of those may take effect.
  The suite hammers this from every angle.
- **Throw-after-callback (2.3.3.3.4).** If `then` calls `resolve(y)` and *then*
  throws, the throw is swallowed — the resolution already happened. The guard is
  `if (!called) reject(err)`.

## Why `resolvePromise` recurses

A thenable can resolve with another thenable, which can resolve with a promise,
which can resolve with a value. Each `resolve` callback calls back into
`resolvePromise`, so the chain unwinds to a single final settlement. The
recursion depth is bounded by the length of the thenable chain, which is data,
not pattern — and the deep-chain test confirms an ordinary 2000-link `then`
chain does not overflow, because those links schedule on microtasks rather than
nesting on the stack.

## Interop with native promises

Because `APromise` exposes a spec-correct `then`, it *is* a thenable, so a native
`Promise` resolving with an `APromise` runs its own §2.3 and adopts our state.
That is why the behavioural tests bridge with `Promise.resolve(aPromise)` and
then `await` — no special adapter, just the specification working in both
directions. The conformance adapter (`test/run-aplus.mjs`) is likewise tiny:
`resolved`, `rejected`, and a `deferred` that exposes the executor's
`resolve`/`reject`.

## Files

| file | role |
|---|---|
| `src/promise.js` | `APromise` and the §2.3 resolution procedure |
| `test/promise.test.js` | 18 behavioural tests (`node --test`, no install) |
| `test/run-aplus.mjs` | the official 872-test conformance runner |

## Beyond A+

`resolve`, `reject`, `all`, `race`, `catch`, `finally` are ECMAScript additions,
not A+ — implemented on top of the conformant core the same way the language
does. `finally`, for instance, is `then` with a callback that ignores its
argument and re-throws on the rejection path, so the value or reason passes
through unchanged. Keeping them layered on top, rather than woven in, is what
lets the 872-test core stay exactly the specification and nothing more.
