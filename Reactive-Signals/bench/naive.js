// A naive eager push-based reactive system, built only to be the foil in the
// benchmark. This is what almost everyone writes first, and what a surprising
// number of shipped systems actually do: on a write, recompute everything
// downstream immediately, in the order dependencies were registered.
//
// It is wrong in two measurable ways, both of which bench/diamond.js exercises:
//
//   1. Glitches. A join node is recomputed once per parent update, so it briefly
//      holds a value computed from one fresh parent and one stale one.
//   2. Exponential work on a diamond chain. Nothing is memoised and nothing is
//      deduplicated, so a single write re-triggers the whole downstream cone
//      once per path — and the number of paths through D stacked diamonds is
//      2^D.
//
// It is kept deliberately small and obvious; the point is that the obvious thing
// is the slow, incorrect thing.

export const naiveStats = { computations: 0 };

let currentObserver = null;

class NaiveSignal {
  constructor(value) {
    this.value = value;
    this.observers = [];
  }
  get() {
    if (currentObserver && !this.observers.includes(currentObserver)) {
      this.observers.push(currentObserver);
    }
    return this.value;
  }
  set(value) {
    this.value = value;
    // Eagerly recompute every dependent, right now, no questions asked.
    for (const o of this.observers.slice()) o.recompute();
  }
}

class NaiveComputed {
  constructor(fn) {
    this.fn = fn;
    this.observers = [];
    this.recompute(true);
  }
  recompute(initial = false) {
    const prev = currentObserver;
    currentObserver = this;
    naiveStats.computations++;
    this.value = this.fn();
    currentObserver = prev;
    // Push the (possibly glitched) new value straight to dependents. On a
    // diamond chain this fires the same downstream node over and over.
    if (!initial) {
      for (const o of this.observers.slice()) o.recompute();
    }
  }
  get() {
    if (currentObserver && !this.observers.includes(currentObserver)) {
      this.observers.push(currentObserver);
    }
    return this.value;
  }
}

class NaiveEffect {
  constructor(fn) {
    this.fn = fn;
    this.recompute();
  }
  recompute() {
    const prev = currentObserver;
    currentObserver = this;
    naiveStats.computations++;
    this.fn();
    currentObserver = prev;
  }
}

// Expose the same callable-accessor API as the real library, so the benchmark
// can build both graphs with identical code.
export const naive = {
  signal(v) {
    const node = new NaiveSignal(v);
    const accessor = () => node.get();
    accessor.set = (x) => node.set(x);
    return accessor;
  },
  computed(fn) {
    const node = new NaiveComputed(fn);
    return () => node.get();
  },
  effect(fn) {
    new NaiveEffect(fn);
  },
};
