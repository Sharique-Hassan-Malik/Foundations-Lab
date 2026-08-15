// Behavioural tests, complementing the official Promises/A+ suite.
//
// The A+ suite (test/run-aplus.mjs, 872 tests) proves the resolution procedure
// is conformant. These tests cover the things A+ deliberately leaves out — the
// async-scheduling guarantee, and the non-spec conveniences every real Promise
// has (resolve/reject/all/race/finally) — and serve as a fast `node --test`
// smoke check that needs no network install.
import assert from "node:assert/strict";
import { test } from "node:test";
import { APromise } from "../src/promise.js";

const tick = () => new Promise((r) => setTimeout(r, 0));

test("fulfils with a value", async () => {
  const p = new APromise((resolve) => resolve(42));
  assert.equal(await toNative(p), 42);
});

test("rejects with a reason", async () => {
  const p = new APromise((_, reject) => reject(new Error("nope")));
  await assert.rejects(toNative(p), /nope/);
});

test("an executor that throws rejects the promise", async () => {
  const p = new APromise(() => {
    throw new Error("boom");
  });
  await assert.rejects(toNative(p), /boom/);
});

test("handlers run asynchronously, after then() returns", async () => {
  const order = [];
  const p = new APromise((resolve) => resolve("v"));
  p.then(() => order.push("handler"));
  order.push("after then");
  await tick();
  assert.deepEqual(order, ["after then", "handler"], "handler must not run in then()'s frame");
});

test("chaining transforms the value", async () => {
  const p = APromise.resolve(1)
    .then((x) => x + 1)
    .then((x) => x * 10);
  assert.equal(await toNative(p), 20);
});

test("a thrown error skips to the next rejection handler", async () => {
  const seen = [];
  const p = APromise.resolve()
    .then(() => {
      throw new Error("x");
    })
    .then(() => seen.push("should be skipped"))
    .catch((e) => seen.push("caught " + e.message));
  await toNative(p);
  assert.deepEqual(seen, ["caught x"]);
});

test("resolving with a promise adopts its state", async () => {
  const inner = new APromise((resolve) => setTimeout(() => resolve("inner"), 5));
  const outer = new APromise((resolve) => resolve(inner));
  assert.equal(await toNative(outer), "inner");
});

test("resolving with a foreign thenable adopts its state", async () => {
  const thenable = { then: (resolve) => resolve("from thenable") };
  assert.equal(await toNative(APromise.resolve(thenable)), "from thenable");
});

test("a self-resolving promise rejects with a TypeError (cycle)", async () => {
  let resolveSelf;
  const p = new APromise((resolve) => {
    resolveSelf = resolve;
  });
  resolveSelf(p); // resolve with itself
  await assert.rejects(toNative(p), TypeError);
});

test("settle-once: a second resolve or reject is ignored", async () => {
  const p = new APromise((resolve, reject) => {
    resolve("first");
    resolve("second");
    reject("third");
  });
  assert.equal(await toNative(p), "first");
});

test("catch handles rejection", async () => {
  const p = APromise.reject(new Error("e")).catch((e) => "recovered " + e.message);
  assert.equal(await toNative(p), "recovered e");
});

test("finally runs on both paths and passes the value through", async () => {
  let calls = 0;
  const okValue = await toNative(APromise.resolve("ok").finally(() => calls++));
  assert.equal(okValue, "ok");
  await assert.rejects(toNative(APromise.reject(new Error("bad")).finally(() => calls++)), /bad/);
  assert.equal(calls, 2, "finally ran on fulfil and on reject");
});

test("all resolves to the array of values in order", async () => {
  const p = APromise.all([
    APromise.resolve(1),
    new APromise((r) => setTimeout(() => r(2), 5)),
    3,
  ]);
  assert.deepEqual(await toNative(p), [1, 2, 3]);
});

test("all rejects as soon as any input rejects", async () => {
  const p = APromise.all([APromise.resolve(1), APromise.reject(new Error("fail")), APromise.resolve(3)]);
  await assert.rejects(toNative(p), /fail/);
});

test("all of an empty iterable resolves to an empty array", async () => {
  assert.deepEqual(await toNative(APromise.all([])), []);
});

test("race settles with the first to settle", async () => {
  const slow = new APromise((r) => setTimeout(() => r("slow"), 20));
  const fast = new APromise((r) => setTimeout(() => r("fast"), 1));
  assert.equal(await toNative(APromise.race([slow, fast])), "fast");
});

test("a non-function executor throws synchronously", () => {
  assert.throws(() => new APromise(42), TypeError);
});

test("deep synchronous chains do not overflow", async () => {
  let p = APromise.resolve(0);
  for (let i = 0; i < 2000; i++) p = p.then((x) => x + 1);
  assert.equal(await toNative(p), 2000);
});

// Bridge our promise to a native one so `await` and assert.rejects can drive it.
// (Our promise is a genuine thenable, so a native promise adopts it directly.)
function toNative(aPromise) {
  return Promise.resolve(aPromise);
}
