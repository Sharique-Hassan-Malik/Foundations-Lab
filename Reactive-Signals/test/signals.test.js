import assert from "node:assert/strict";
import { test } from "node:test";
import { batch, computed, effect, signal, stats, untrack } from "../src/signals.js";

test("signal holds and updates a value", () => {
  const count = signal(0);
  assert.equal(count(), 0);
  count.set(5);
  assert.equal(count(), 5);
});

test("computed derives from a signal and stays current", () => {
  const n = signal(2);
  const doubled = computed(() => n() * 2);
  assert.equal(doubled(), 4);
  n.set(10);
  assert.equal(doubled(), 20);
});

test("computed is memoised: it does not recompute when read again", () => {
  const n = signal(1);
  let runs = 0;
  const c = computed(() => {
    runs++;
    return n() + 1;
  });
  c();
  c();
  c();
  assert.equal(runs, 1, "three reads, one computation");
  n.set(2);
  c();
  assert.equal(runs, 2, "one write, one more computation");
});

test("effect runs immediately and again on change", () => {
  const n = signal(0);
  const seen = [];
  effect(() => seen.push(n()));
  assert.deepEqual(seen, [0]);
  n.set(1);
  n.set(2);
  assert.deepEqual(seen, [0, 1, 2]);
});

test("effect does not run when a write does not change the value", () => {
  const n = signal(1);
  let runs = 0;
  effect(() => {
    n();
    runs++;
  });
  assert.equal(runs, 1);
  n.set(1); // same value
  assert.equal(runs, 1, "Object.is-equal write is a no-op");
});

test("the diamond recomputes the sink exactly once, with no glitch", () => {
  const a = signal(1);
  const b = computed(() => a() + 1);
  const c = computed(() => a() + 1);
  let sinkRuns = 0;
  const seen = [];
  const d = computed(() => {
    sinkRuns++;
    return b() + c();
  });
  effect(() => seen.push(d()));
  assert.deepEqual(seen, [4]);

  sinkRuns = 0;
  a.set(10);
  assert.equal(sinkRuns, 1, "sink recomputed once, not once per parent");
  assert.deepEqual(seen, [4, 22], "no impossible intermediate value was observed");
});

test("equality cutoff stops propagation when a derived value is unchanged", () => {
  const n = signal(1);
  const isPositive = computed(() => n() > 0);
  let effectRuns = 0;
  effect(() => {
    isPositive();
    effectRuns++;
  });
  assert.equal(effectRuns, 1);
  n.set(2); // still positive: isPositive stays true
  n.set(3); // still positive
  assert.equal(effectRuns, 1, "downstream effect never re-ran because the boolean did not change");
  n.set(-1); // now false
  assert.equal(effectRuns, 2);
});

test("a wide diamond is linear, not quadratic, in the number of nodes", () => {
  const width = 50;
  const source = signal(0);
  const middles = Array.from({ length: width }, (_, i) => computed(() => source() + i));
  const sink = computed(() => middles.reduce((sum, m) => sum + m(), 0));
  effect(() => sink());

  const before = stats.computations;
  source.set(1);
  const work = stats.computations - before;
  // width middles + one sink + one effect = width + 2. A per-path system would
  // re-run the sink `width` times.
  assert.ok(work <= width + 3, `did ${work} computations for width ${width}, expected ~${width + 2}`);
});

test("dependencies are dynamic: a branch not taken creates no dependency", () => {
  const cond = signal(true);
  const x = signal("x");
  const y = signal("y");
  let runs = 0;
  const c = computed(() => {
    runs++;
    return cond() ? x() : y();
  });
  assert.equal(c(), "x");
  assert.equal(runs, 1);

  y.set("Y"); // c does not read y while cond is true
  c();
  assert.equal(runs, 1, "changing an untaken branch must not invalidate");

  cond.set(false); // now c reads y
  assert.equal(c(), "Y");
  x.set("X"); // and no longer reads x
  c();
  assert.equal(c(), "Y");
});

test("batch coalesces writes so effects run once", () => {
  const a = signal(1);
  const b = signal(2);
  let runs = 0;
  const seen = [];
  effect(() => {
    seen.push(a() + b());
    runs++;
  });
  assert.equal(runs, 1);
  batch(() => {
    a.set(10);
    b.set(20);
  });
  assert.equal(runs, 2, "two writes in a batch, one effect run");
  assert.deepEqual(seen, [3, 30]);
});

test("untrack reads without subscribing", () => {
  const a = signal(1);
  const b = signal(2);
  let runs = 0;
  effect(() => {
    a();
    untrack(() => b());
    runs++;
  });
  assert.equal(runs, 1);
  b.set(99); // read under untrack: no subscription
  assert.equal(runs, 1);
  a.set(2);
  assert.equal(runs, 2);
});

test("effect cleanup runs before re-execution and on dispose", () => {
  const n = signal(0);
  const events = [];
  const dispose = effect(() => {
    const value = n();
    events.push(`run ${value}`);
    return () => events.push(`cleanup ${value}`);
  });
  n.set(1);
  n.set(2);
  dispose();
  assert.deepEqual(events, [
    "run 0",
    "cleanup 0",
    "run 1",
    "cleanup 1",
    "run 2",
    "cleanup 2", // on dispose
  ]);
  n.set(3); // disposed: nothing more
  assert.deepEqual(events.at(-1), "cleanup 2");
});

test("a disposed effect stops observing", () => {
  const n = signal(0);
  let runs = 0;
  const dispose = effect(() => {
    n();
    runs++;
  });
  n.set(1);
  assert.equal(runs, 2);
  dispose();
  n.set(2);
  n.set(3);
  assert.equal(runs, 2, "no runs after dispose");
});

test("computeds compose through several layers", () => {
  const base = signal(1);
  const a = computed(() => base() * 2);
  const b = computed(() => a() + 1);
  const c = computed(() => b() * b());
  assert.equal(c(), 9); // ((1*2)+1)^2
  base.set(3);
  assert.equal(c(), 49); // ((3*2)+1)^2
});

test("peek reads a signal without tracking", () => {
  const n = signal(5);
  let runs = 0;
  effect(() => {
    n.peek(); // does not subscribe
    runs++;
  });
  assert.equal(runs, 1);
  n.set(6);
  assert.equal(runs, 1, "peek did not create a dependency");
  assert.equal(n.peek(), 6);
});

test("a computed is read-only: it exposes no setter", () => {
  const n = signal(1);
  const c = computed(() => n() + 1);
  assert.equal(typeof c.set, "undefined", "computeds are derived, not writable");
  assert.equal(typeof signal(0).set, "function", "signals are writable");
});

test("a stress diamond stays glitch-free under many writes", () => {
  const a = signal(0);
  const b = computed(() => a() + 1);
  const c = computed(() => a() * 2);
  const d = computed(() => b() * 100 + c()); // encodes both inputs unambiguously
  const violations = [];
  effect(() => {
    const av = a();
    const expected = (av + 1) * 100 + av * 2;
    if (d() !== expected) violations.push({ av, got: d(), expected });
  });
  for (let i = 1; i <= 1000; i++) a.set(i);
  assert.deepEqual(violations, [], "every observed d matched its inputs exactly");
});
