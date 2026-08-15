// Runs the official Promises/A+ conformance suite against our implementation.
//
//   node test/run-aplus.mjs
//
// The suite (the `promises-aplus-tests` package) is the reference: 872 tests
// covering every clause of the specification. It drives an implementation
// through a small adapter — a way to make a resolved promise, a rejected one,
// and a "deferred" (a promise plus its resolve/reject). If all 872 pass, the
// implementation is conformant by definition.
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const promisesAplusTests = require("promises-aplus-tests");
const { APromise } = await import("../src/promise.js");

const adapter = {
  resolved: (value) => APromise.resolve(value),
  rejected: (reason) => APromise.reject(reason),
  deferred: () => {
    let resolve, reject;
    const promise = new APromise((res, rej) => {
      resolve = res;
      reject = rej;
    });
    return { promise, resolve, reject };
  },
};

promisesAplusTests(adapter, (err) => {
  process.exit(err ? 1 : 0);
});
