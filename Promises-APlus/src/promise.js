// A Promise, built to the Promises/A+ specification (https://promisesaplus.com).
//
// The spec is short and almost the entire difficulty is one procedure — the
// Promise Resolution Procedure, §2.3 — which decides what it means to resolve a
// promise with an arbitrary value that might itself be a promise, a foreign
// "thenable", a plain object, or nothing special at all. Every subtle rule in
// the 872-test conformance suite is a corner of that procedure: cycles,
// thenables that throw from a getter, thenables that call back twice, thenables
// whose `then` is a value rather than a function.
//
// Two invariants make the rest fall into place, and both are spec requirements
// rather than style:
//
//   * A promise settles at most once. Every path that could settle it goes
//     through a guard so the second attempt is a no-op. Without this, a
//     malicious or buggy thenable that calls both its callbacks corrupts state.
//
//   * Handlers run asynchronously, after the call to `then` returns (§2.2.4).
//     `queueMicrotask` schedules them, so `then` never runs a callback in its
//     own stack frame — code after `.then(fn)` always runs before `fn`.

const PENDING = 0;
const FULFILLED = 1;
const REJECTED = 2;

export class APromise {
  #state = PENDING;
  #value = undefined; // fulfilment value or rejection reason
  #callbacks = []; // { onFulfilled, onRejected, resolve, reject } queued while pending

  /**
   * @param {(resolve: (v?) => void, reject: (r?) => void) => void} executor
   */
  constructor(executor) {
    if (typeof executor !== "function") {
      throw new TypeError("Promise executor must be a function");
    }
    // If the executor throws before settling, that throw rejects the promise —
    // but a throw after it has already settled is ignored, hence the guard.
    try {
      executor(
        (value) => this.#resolve(value),
        (reason) => this.#reject(reason),
      );
    } catch (err) {
      this.#reject(err);
    }
  }

  // §2.2 — the only method the spec defines.
  then(onFulfilled, onRejected) {
    // A new promise to represent the result of the handlers (§2.2.7).
    let resolveNext;
    let rejectNext;
    const next = new APromise((resolve, reject) => {
      resolveNext = resolve;
      rejectNext = reject;
    });

    const handler = {
      // Non-function handlers must be ignored, and the value/reason must pass
      // through to `next` unchanged (§2.2.1, §2.2.7.3–4). That pass-through is
      // what makes `.then(fn)` on a rejected promise still propagate the error.
      onFulfilled: typeof onFulfilled === "function" ? onFulfilled : null,
      onRejected: typeof onRejected === "function" ? onRejected : null,
      resolve: resolveNext,
      reject: rejectNext,
    };

    if (this.#state === PENDING) {
      this.#callbacks.push(handler);
    } else {
      // Already settled: schedule the handler on a microtask, not inline.
      this.#schedule(handler);
    }
    return next;
  }

  // ---- settling ---------------------------------------------------------

  #resolve(value) {
    if (this.#state !== PENDING) return; // settle-once
    // Resolving with a promise or thenable means *adopting* its eventual state,
    // not fulfilling with it. That is the resolution procedure, below.
    resolvePromise(this, value, (v) => this.#fulfill(v), (r) => this.#reject(r));
  }

  #fulfill(value) {
    if (this.#state !== PENDING) return;
    this.#state = FULFILLED;
    this.#value = value;
    this.#flush();
  }

  #reject(reason) {
    if (this.#state !== PENDING) return;
    this.#state = REJECTED;
    this.#value = reason;
    this.#flush();
  }

  #flush() {
    for (const handler of this.#callbacks) this.#schedule(handler);
    this.#callbacks = [];
  }

  // Run one handler asynchronously and route its outcome into the next promise.
  #schedule(handler) {
    const state = this.#state;
    const value = this.#value;
    queueMicrotask(() => {
      const callback = state === FULFILLED ? handler.onFulfilled : handler.onRejected;
      if (callback === null) {
        // No handler for this outcome: pass the value/reason straight through.
        if (state === FULFILLED) handler.resolve(value);
        else handler.reject(value);
        return;
      }
      try {
        // The handler's return value resolves `next` — and if it returns a
        // thenable, `next` adopts *its* state, recursively (§2.2.7.1).
        handler.resolve(callback(value));
      } catch (err) {
        // A handler that throws rejects `next` with the thrown value (§2.2.7.2).
        handler.reject(err);
      }
    });
  }

  // ---- conveniences (not part of A+, but expected of a real Promise) ----

  catch(onRejected) {
    return this.then(null, onRejected);
  }

  finally(onFinally) {
    return this.then(
      (value) => APromise.resolve(onFinally()).then(() => value),
      (reason) =>
        APromise.resolve(onFinally()).then(() => {
          throw reason;
        }),
    );
  }

  static resolve(value) {
    if (value instanceof APromise) return value;
    return new APromise((resolve) => resolve(value));
  }

  static reject(reason) {
    return new APromise((_, reject) => reject(reason));
  }

  static all(iterable) {
    return new APromise((resolve, reject) => {
      const results = [];
      let remaining = 0;
      let index = 0;
      for (const item of iterable) {
        const i = index++;
        remaining++;
        APromise.resolve(item).then((value) => {
          results[i] = value;
          if (--remaining === 0) resolve(results);
        }, reject);
      }
      if (index === 0) resolve([]);
    });
  }

  static race(iterable) {
    return new APromise((resolve, reject) => {
      for (const item of iterable) APromise.resolve(item).then(resolve, reject);
    });
  }
}

/**
 * The Promise Resolution Procedure, §2.3 — `[[Resolve]](promise, x)`.
 *
 * This is where every hard case lives. It is a free function rather than a
 * method because it operates on the *fulfil/reject* capabilities, not on the
 * promise's internals, and because a thenable's `then` may resolve with yet
 * another thenable, so the procedure must recurse through `fulfil` calling back
 * into `resolvePromise`.
 */
function resolvePromise(promise, x, fulfil, reject) {
  // §2.3.1 — a promise resolved with itself is a cycle; reject with a TypeError.
  if (x === promise) {
    reject(new TypeError("Chaining cycle detected: a promise cannot resolve to itself"));
    return;
  }

  // §2.3.3 — x is an object or function: it might be a thenable.
  if (x !== null && (typeof x === "object" || typeof x === "function")) {
    let then;
    try {
      // §2.3.3.1 — reading `.then` can throw (a getter). If it does, reject.
      then = x.then;
    } catch (err) {
      reject(err);
      return;
    }

    if (typeof then === "function") {
      // x is a thenable. Adopt its state — but its `then` is untrusted code that
      // may call back many times or both ways, so guard to honour the first call
      // only (§2.3.3.3.3). This is the single most-tested rule in the suite.
      let called = false;
      try {
        then.call(
          x,
          (y) => {
            if (called) return;
            called = true;
            // The thenable resolved with y, which may itself be a thenable:
            // recurse, so a chain of promises collapses to one final value.
            resolvePromise(promise, y, fulfil, reject);
          },
          (r) => {
            if (called) return;
            called = true;
            reject(r);
          },
        );
      } catch (err) {
        // §2.3.3.3.4 — if `then` throws after having already called back, the
        // callback wins and the throw is ignored; otherwise the throw rejects.
        if (!called) reject(err);
      }
      return;
    }
    // §2.3.3.4 — `.then` is a value, not a function: x is an ordinary object.
    fulfil(x);
    return;
  }

  // §2.3.4 — x is a primitive (or null): fulfil with it directly.
  fulfil(x);
}
