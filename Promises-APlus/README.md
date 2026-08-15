# Promises/A+ From Scratch

A JavaScript Promise built to the [Promises/A+ specification](https://promisesaplus.com),
and **verified against the official 872-test conformance suite** — the same
suite the specification's authors use to certify a compliant implementation.

---

## The headline

```
$ npm run conformance

  ...
  872 passing (13s)
```

All 872 official conformance tests pass. That is not a claim about how careful
the code is — it is the specification's own executable definition of "correct",
run against this implementation. Every subtle clause is a test: cycles,
thenables that throw from a `.then` getter, thenables that call back twice,
thenables whose `then` is a value rather than a function, adoption of pending
state, the exact asynchrony of handler scheduling.

---

## Why a Promise is harder than it looks

`then`, three states, a queue of callbacks — that part is an afternoon. The
difficulty is one procedure: **the Promise Resolution Procedure (§2.3)**, which
answers "what does it mean to resolve a promise with *this* value?" when the
value might be another promise, a foreign thenable from a different library, a
plain object, or nothing special. Almost the entire conformance suite lives in
the corners of that one procedure.

Two invariants carry the design, and both are spec requirements, not taste:

**Settle at most once.** Every path that could settle a promise passes through a
guard, so a second `resolve`, or a thenable that calls both its callbacks, is a
no-op:

```js
resolve("first");
resolve("second");   // ignored
reject("third");     // ignored
// → the promise fulfils with "first"
```

**Handlers are asynchronous.** A handler never runs in the stack frame that
registered it (§2.2.4). Code after `.then(fn)` always runs before `fn`:

```js
const order = [];
promise.then(() => order.push("handler"));
order.push("after then");
// → ["after then", "handler"]
```

`queueMicrotask` schedules handlers, so this holds without a busy loop or a
`setTimeout`.

## The resolution procedure, annotated

The hard case is a **thenable** — any object with a `then` method, possibly from
another Promise library, possibly hostile. The spec says: call its `then`, adopt
whatever it does, but honour only the *first* callback it makes, and if reading
`.then` or calling it throws, reject — unless a callback already fired, in which
case the callback wins.

```js
let called = false;                       // the single most-tested rule
try {
  then.call(x,
    (y) => { if (called) return; called = true; resolvePromise(promise, y, ...); },
    (r) => { if (called) return; called = true; reject(r); });
} catch (err) {
  if (!called) reject(err);               // a throw after a callback is ignored
}
```

The inner `resolvePromise(promise, y, ...)` is a recursive call: a thenable that
resolves with another thenable collapses the whole chain to one final value.
[ARCHITECTURE.md](./ARCHITECTURE.md) walks every clause.

## Usage

```js
import { APromise } from "./src/promise.js";

new APromise((resolve, reject) => {
  setTimeout(() => resolve(42), 100);
})
  .then((x) => x + 1)
  .then((x) => console.log(x));      // 43

APromise.resolve(1);
APromise.reject(new Error("no"));
APromise.all([p1, p2, p3]);          // array of results, or first rejection
APromise.race([slow, fast]);         // first to settle
p.catch(fn);
p.finally(fn);
```

The implementation is a genuine thenable, so it interoperates with native
promises — `await new APromise(...)` and `Promise.all([native, aPromise])` both
work, because a native promise adopts ours through the same procedure.

## Running it

```bash
npm install          # fetches the official promises-aplus-tests suite (dev only)
npm test             # 18 behavioural tests via node --test — no install needed
npm run conformance  # the official 872-test suite
```

`npm test` covers the parts A+ deliberately leaves out — the async-scheduling
guarantee and the non-spec conveniences (`resolve`/`reject`/`all`/`race`/
`finally`) — and needs no network. `npm run conformance` is the specification's
own suite and needs the `npm install` first.

## What A+ does and does not cover

The specification is deliberately minimal: it standardises `then` and the
resolution procedure, and **nothing else**. `resolve`, `reject`, `all`, `race`,
`catch` and `finally` are not part of A+ — they are what ECMAScript later layered
on top, and they are implemented here on top of the conformant core exactly as
the language does, which is why the behavioural suite tests them separately.

Not implemented, because they are runtime concerns rather than spec ones:
unhandled-rejection tracking (needs host integration) and `Promise.any` /
`allSettled` (trivial additions of the same shape as `all`, omitted to keep the
surface to what illustrates the algorithm).

## License

MIT
