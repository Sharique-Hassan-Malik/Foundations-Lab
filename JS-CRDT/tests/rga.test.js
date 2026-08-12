import { test } from 'node:test';
import assert from 'node:assert/strict';
import { RGA } from '../src/rga.js';

// a small seeded PRNG so failures are reproducible
function mulberry32(seed) {
  return function () {
    seed |= 0; seed = (seed + 0x6D2B79F5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
function shuffle(arr, rng) {
  for (let i = arr.length - 1; i > 0; i--) {
    const j = Math.floor(rng() * (i + 1));
    [arr[i], arr[j]] = [arr[j], arr[i]];
  }
  return arr;
}

test('basic local editing', () => {
  const d = new RGA('A');
  d.insertAt(0, 'h');
  d.insertAt(1, 'i');
  d.insertAt(1, 'e');           // "hei"
  assert.equal(d.toString(), 'hei');
  d.deleteAt(1);                // remove 'e' -> "hi"
  assert.equal(d.toString(), 'hi');
  assert.equal(d.length, 2);
});

test('applying an operation twice is a no-op (idempotent)', () => {
  const a = new RGA('A');
  const op = a.insertAt(0, 'x');
  const b = new RGA('B');
  b.apply(op);
  b.apply(op);                  // duplicate delivery
  assert.equal(b.toString(), 'x');
});

test('out-of-order delivery is buffered until dependencies arrive', () => {
  const a = new RGA('A');
  const o1 = a.insertAt(0, 'a');   // after ROOT
  const o2 = a.insertAt(1, 'b');   // after o1
  const o3 = a.insertAt(2, 'c');   // after o2

  const b = new RGA('B');
  b.apply(o3);                     // depends on o2 (absent) -> buffered
  b.apply(o2);                     // depends on o1 (absent) -> buffered
  assert.equal(b.toString(), '');  // nothing visible yet
  b.apply(o1);                     // unblocks o2, which unblocks o3
  assert.equal(b.toString(), 'abc');
});

test('concurrent inserts at the same position converge on both replicas', () => {
  const a = new RGA('A');
  const b = new RGA('B');
  const oa = a.insertAt(0, 'A');   // both insert at position 0 concurrently
  const ob = b.insertAt(0, 'B');
  a.apply(ob);
  b.apply(oa);
  assert.equal(a.toString(), b.toString(), 'both replicas agree on the tie-break order');
});

test('HEADLINE: replicas converge under random delivery order (strong eventual consistency)', () => {
  for (let trial = 0; trial < 60; trial++) {
    const rng = mulberry32(trial * 7 + 1);
    const N = 3;
    const replicas = Array.from({ length: N }, (_, i) => new RGA(String.fromCharCode(65 + i)));
    const ops = [];

    // each step, a random replica makes a random local edit
    for (let step = 0; step < 50; step++) {
      const rep = replicas[Math.floor(rng() * N)];
      const len = rep.length;
      if (len === 0 || rng() < 0.7) {
        const idx = Math.floor(rng() * (len + 1));
        const ch = String.fromCharCode(97 + Math.floor(rng() * 26));
        ops.push(rep.insertAt(idx, ch));
      } else {
        const op = rep.deleteAt(Math.floor(rng() * len));
        if (op) ops.push(op);
      }
    }

    // deliver every op to every replica in an independently shuffled order
    for (const rep of replicas) {
      for (const op of shuffle(ops.slice(), rng)) rep.apply(op);
    }

    // all replicas render the identical sequence, and nothing is left buffered
    const text = replicas[0].toString();
    for (let i = 1; i < N; i++) {
      assert.equal(replicas[i].toString(), text, `trial ${trial}: replica ${i} diverged`);
    }
    for (const rep of replicas) {
      const stuck = [...rep.pending.values()].reduce((n, l) => n + l.length, 0);
      assert.equal(stuck, 0, `trial ${trial}: ops left buffered`);
    }
  }
});
