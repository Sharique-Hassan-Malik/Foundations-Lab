import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GCounter, PNCounter } from '../src/counters.js';

test('GCounter sums per-replica increments', () => {
  const a = new GCounter('A');
  a.increment().increment(4);
  assert.equal(a.value(), 5);
});

test('GCounter merge is the element-wise max and converges both ways', () => {
  const a = new GCounter('A');
  const b = new GCounter('B');
  a.increment(3);
  b.increment(10);
  const ab = a.clone().merge(b);
  const ba = b.clone().merge(a);
  assert.equal(ab.value(), 13);
  assert.equal(ba.value(), 13);
  assert.ok(ab.equals(ba), 'merge is commutative');
});

test('GCounter merge is idempotent', () => {
  const a = new GCounter('A').increment(2);
  const b = new GCounter('B').increment(7);
  const once = a.clone().merge(b);
  const twice = a.clone().merge(b).merge(b);
  assert.ok(once.equals(twice), 'merging the same state twice changes nothing');
});

test('PNCounter supports decrement and converges', () => {
  const a = new PNCounter('A');
  const b = new PNCounter('B');
  a.increment(10).decrement(3);   // A: +7
  b.increment(5).decrement(9);    // B: -4
  const merged = a.clone().merge(b);
  assert.equal(merged.value(), 3);                 // 7 + (-4)
  assert.ok(merged.equals(b.clone().merge(a)), 'order of merge does not matter');
});
