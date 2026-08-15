// Fine-grained reactivity: signal / computed / effect, glitch-free.
//
// The problem this solves is the "diamond". Given
//
//     a = signal(1)
//     b = computed(() => a() + 1)
//     c = computed(() => a() + 1)
//     d = computed(() => b() + c())      // depends on a through two paths
//
// a naive push-based system reacts to `a` changing by recomputing everything
// that reads `a`, transitively, in the order dependencies were registered. That
// recomputes `d` *twice* (once when `b` updates, once when `c` updates), and —
// worse — the first of those recomputations reads the new `b` and the *old* `c`,
// so `d` briefly holds a value that is arithmetically impossible. That transient
// wrong value is a "glitch", and if an effect observes it, the glitch escapes.
//
// This library is push-then-pull. A write only *marks* the graph, cheaply and
// without recomputing anything. Values are recomputed lazily, when read, and a
// node about to recompute first brings all of its sources up to date — so when
// `d` finally recomputes it sees a consistent `b` and `c`, exactly once. The
// same mechanism gives an equality cutoff for free: if a recomputation produces
// the same value, nothing downstream is disturbed.
//
// The algorithm is the three-colour marking scheme (clean / check / dirty) used
// by modern signal libraries. The comments below say what each colour means and
// why the transitions are what they are.

const CLEAN = 0; // value is current
const CHECK = 1; // a transitive source changed; this node *might* be stale
const DIRTY = 2; // a direct source changed; this node *is* stale

// The reaction currently executing, so that a signal read inside it can record
// the dependency. A stack, because computeds nest.
let currentReaction = null;
// When non-null, reads are *not* tracked. Used by untrack() and by effect
// cleanup, where reading a value must not create a dependency.
let currentlyUntracked = false;

// Effects that need to run after the current write settles. A set, so an effect
// invalidated by several signals in one batch still runs once.
let pendingEffects = [];
let batchDepth = 0;

// Instrumentation: total number of computed/effect function invocations. The
// benchmark and tests assert on this, because "recomputed once, not twice" is
// the whole correctness claim and it is only visible as a count.
export const stats = { computations: 0 };

class Reactive {
  constructor(fnOrValue, isEffect = false) {
    this.observers = new Set(); // reactions that read this node
    this.sources = new Set(); // nodes this reaction read on its last run
    if (typeof fnOrValue === "function") {
      this.fn = fnOrValue;
      this.value = undefined;
      this.state = DIRTY; // never computed yet
      this.isEffect = isEffect;
      this.cleanups = [];
      if (isEffect) {
        // An effect has no reader to pull it, so it must be scheduled. Run it
        // once now (outside any batch) to establish its dependencies.
        if (batchDepth > 0) pendingEffects.push(this);
        else this.updateIfNecessary();
      }
    } else {
      this.fn = null; // a plain signal
      this.value = fnOrValue;
      this.state = CLEAN;
    }
  }

  // ---- read -------------------------------------------------------------

  get() {
    // Record the dependency edge in both directions, unless we are untracking.
    if (currentReaction && !currentlyUntracked) {
      currentReaction.sources.add(this);
      this.observers.add(currentReaction);
    }
    if (this.fn) this.updateIfNecessary();
    return this.value;
  }

  // ---- write (signals only) --------------------------------------------

  set(next) {
    if (this.fn) throw new Error("cannot set() a computed; only signals are writable");
    if (Object.is(next, this.value)) return; // no change, no work — the first cutoff
    this.value = next;
    // Direct observers are definitely stale; their observers only *maybe*.
    for (const o of this.observers) o.markStale(DIRTY);
    flush();
  }

  // ---- marking ----------------------------------------------------------

  // Push phase: propagate staleness upward without recomputing anything.
  //
  // A node only ever moves toward "more stale" (CLEAN -> CHECK -> DIRTY), and if
  // it was already at least this stale the propagation stops — that early exit
  // is what keeps a wide diamond linear instead of quadratic. A node newly made
  // stale tells *its* observers they might be affected (CHECK), and if it is an
  // effect it joins the queue to be pulled after the write settles.
  markStale(state) {
    if (this.state >= state) return;
    const wasClean = this.state === CLEAN;
    this.state = state;
    if (wasClean && this.isEffect) pendingEffects.push(this);
    for (const o of this.observers) o.markStale(CHECK);
  }

  // Pull phase: make this node's value current, doing the minimum work.
  updateIfNecessary() {
    if (this.state === CHECK) {
      // "Might be stale." Find out by bringing each source up to date. If any
      // source actually changed its value it will have marked us DIRTY (see
      // update()); the moment that happens we can stop checking the rest.
      for (const source of this.sources) {
        source.updateIfNecessary();
        if (this.state === DIRTY) break;
      }
    }
    if (this.state === DIRTY) this.update();
    // Either we recomputed, or every source was unchanged: we are clean now.
    this.state = CLEAN;
  }

  // Recompute this node's value and re-record its dependencies.
  update() {
    if (this.isEffect) this.runCleanups();

    // A reaction's dependencies can change between runs (a branch not taken this
    // time). Detach from the old sources first; get() will re-attach to whatever
    // is actually read this run.
    for (const source of this.sources) source.observers.delete(this);
    this.sources.clear();

    const prevReaction = currentReaction;
    const prevUntracked = currentlyUntracked;
    currentReaction = this;
    currentlyUntracked = false;
    let next;
    try {
      stats.computations++;
      next = this.fn();
    } finally {
      currentReaction = prevReaction;
      currentlyUntracked = prevUntracked;
    }

    if (this.isEffect) {
      // An effect may return a cleanup function.
      if (typeof next === "function") this.cleanups.push(next);
      return;
    }

    // Equality cutoff: if the value did not actually change, observers stay
    // clean and the diamond stops here. Only a genuine change makes observers
    // DIRTY, which is what they will discover on their own updateIfNecessary.
    if (!Object.is(next, this.value)) {
      this.value = next;
      for (const o of this.observers) o.state = DIRTY;
    }
  }

  runCleanups() {
    if (this.cleanups.length) {
      const prev = currentlyUntracked;
      currentlyUntracked = true;
      try {
        for (const c of this.cleanups) c();
      } finally {
        currentlyUntracked = prev;
        this.cleanups.length = 0;
      }
    }
  }

  dispose() {
    this.runCleanups();
    for (const source of this.sources) source.observers.delete(this);
    this.sources.clear();
    this.observers.clear();
    this.state = CLEAN;
  }
}

// Run every queued effect. Effects pull their own values, which triggers the
// lazy recomputation of exactly the computeds that feed them — and no others.
function flush() {
  if (batchDepth > 0) return; // defer until the batch closes
  while (pendingEffects.length) {
    const effects = pendingEffects;
    pendingEffects = [];
    for (const e of effects) {
      if (e.state !== CLEAN) e.updateIfNecessary();
    }
  }
}

// ---- public API ---------------------------------------------------------

/** A writable reactive value. Returns a getter with a `.set` method. */
export function signal(initial) {
  const node = new Reactive(initial);
  const accessor = () => node.get();
  accessor.set = (v) => node.set(v);
  accessor.peek = () => node.value; // read without tracking
  return accessor;
}

/** A derived value. Recomputed lazily, memoised, and only when its inputs change. */
export function computed(fn) {
  const node = new Reactive(fn);
  const accessor = () => node.get();
  accessor.peek = () => {
    const prev = currentlyUntracked;
    currentlyUntracked = true;
    try {
      return node.get();
    } finally {
      currentlyUntracked = prev;
    }
  };
  return accessor;
}

/**
 * Run `fn` now and again whenever the values it read change. `fn` may return a
 * cleanup function, run before the next invocation and on dispose. Returns a
 * disposer.
 */
export function effect(fn) {
  const node = new Reactive(fn, true);
  return () => node.dispose();
}

/** Read reactive values inside `fn` without creating dependencies. */
export function untrack(fn) {
  const prev = currentlyUntracked;
  currentlyUntracked = true;
  try {
    return fn();
  } finally {
    currentlyUntracked = prev;
  }
}

/**
 * Coalesce multiple writes so effects run once, after all of them. Without this,
 * setting three signals runs dependent effects three times; with it, once.
 */
export function batch(fn) {
  batchDepth++;
  try {
    return fn();
  } finally {
    batchDepth--;
    flush();
  }
}
