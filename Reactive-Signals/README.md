# Reactive Signals

Fine-grained reactivity from scratch — `signal` / `computed` / `effect` — with
**glitch-free, linear-time** updates. The core of what SolidJS and Preact
Signals do, in ~250 lines of dependency-free JavaScript.

---

## Background

Reactivity means: derive values from other values, and keep them current
automatically. The hard part is a shape called the **diamond**, where one source
reaches a node through two paths:

```js
const a = signal(1);
const b = computed(() => a() + 1);
const c = computed(() => a() * 2);
const d = computed(() => `${b()}+${c()}`);   // depends on a via b and via c
```

The obvious implementation — on a write, eagerly recompute everything downstream
— gets this wrong in two ways. It recomputes `d` **twice** (once when `b`
changes, once when `c` does), and the first of those recomputations reads the
new `b` and the *stale* `c`, so `d` briefly holds a value that is arithmetically
impossible. That transient is a **glitch**, and if an effect fires on it — a
network request, a DOM write — the impossible state escapes into the world.

This library never produces a glitch and never does redundant work, using the
three-colour marking scheme (clean / check / dirty) that modern signal libraries
are built on.

---

## The headline

A **chain of D diamonds** — each diamond's output feeds the next. There are
`2^D` paths from the source to the sink, but only `3D` nodes, so the correct
amount of work to settle a change is `O(D)`. Compared against the naive eager
implementation:

```
depth  paths (2^D)  naive evals  real evals
-----  -----------  -----------  ----------
    2            4           13           7
    6           64          253          19
   10         1024         4093          31
   14        16384        65533          43
   18       262144      1048573          55
   20      1048576      4194301          61
```

`npm run bench`

The naive system's work tracks the number of **paths** — it doubles with every
layer (4.2 million recomputations at depth 20). This library's work tracks the
number of **nodes** — 3 per layer, a flat linear 61. Same graph, same result,
different complexity class. It is the [Regex-Engine](https://github.com/Sharique-Hassan-Malik/Regex-Engine) story one
project over: merging reconverging work instead of repeating it.

And the glitch, made visible on a single diamond as `a` goes `1 → 10`:

```
  real   sink observed: ["2+2","11+20"]
  naive  sink observed: ["2+2","11+2","11+20"]
                                └──── impossible: new b, stale c
```

`"11+2"` is a value that should never have existed. The real system emits only
the two correct states.

## How

A write does **not** recompute anything. It cheaply marks the graph — direct
dependents `DIRTY`, their dependents `CHECK` ("might be affected") — and stops
early wherever the marking reaches a node already marked. Values are recomputed
**lazily, when read**, and a node about to recompute first brings its own sources
up to date:

```js
updateIfNecessary() {
  if (this.state === CHECK)            // might be stale — verify by checking sources
    for (const source of this.sources) {
      source.updateIfNecessary();
      if (this.state === DIRTY) break; // a source actually changed; stop early
    }
  if (this.state === DIRTY) this.update();
  this.state = CLEAN;
}
```

So when the sink is finally read, `b` and `c` are already consistent, and it
recomputes **once**. If a recomputation yields the same value (`Object.is`),
nothing downstream is disturbed — the equality cutoff — which is why toggling a
signal between two positive numbers never re-runs an effect that only reads
`n > 0`. See [ARCHITECTURE.md](./ARCHITECTURE.md).

## API

```js
import { signal, computed, effect, batch, untrack } from "./src/signals.js";

const count = signal(0);
const doubled = computed(() => count() * 2);

const dispose = effect(() => {
  console.log("doubled is", doubled());
  return () => console.log("cleanup");   // optional cleanup, run before re-exec and on dispose
});

count.set(5);                 // effect re-runs; logs cleanup then the new value
count.peek();                 // read without subscribing

batch(() => {                 // coalesce writes: effects run once, after both
  count.set(1);
  count.set(2);
});

untrack(() => doubled());     // read without creating a dependency

dispose();                    // stop the effect
```

- **`signal(v)`** — a writable value; call it to read, `.set(v)` to write.
- **`computed(fn)`** — a derived value; lazy, memoised, recomputed only when its
  inputs actually change.
- **`effect(fn)`** — runs now and whenever its reads change; returns a disposer.
- **`batch` / `untrack`** — coalesce writes / read without subscribing.

Dependencies are **dynamic**: a `computed` that takes one branch this run does
not depend on the values in the branch it skipped, so changing them cannot
invalidate it. That falls out of re-tracking sources on every recomputation.

## Running it

```bash
npm test            # 17 tests via node --test — no dependencies
npm run bench       # the diamond-chain and glitch demonstrations
```

Node 18+ (uses the built-in test runner). No `node_modules`, no build step.

## What this is not

- **Not a UI framework.** There is no DOM, no components, no JSX. This is the
  reactivity *core* those are built on; wiring it to the DOM is a separate layer
  (`effect(() => el.textContent = text())` is the whole idea).
- **No async / resources.** Effects are synchronous. Suspense-style async
  reactivity is a real extension and out of scope.
- **Not the fastest possible.** The graph uses `Set`s for clarity; production
  libraries use intrusive linked lists to cut allocation. The complexity class —
  the thing that actually matters — is identical.

## License

MIT
