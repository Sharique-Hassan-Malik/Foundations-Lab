import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ORSet } from '../src/orset.js';

test('add, has, remove, values', () => {
  const s = new ORSet('A');
  s.add('x').add('y');
  assert.ok(s.has('x') && s.has('y'));
  s.remove('x');
  assert.ok(!s.has('x') && s.has('y'));
  assert.deepEqual(s.values(), ['y']);
});

test('re-adding after remove brings the element back', () => {
  const s = new ORSet('A');
  s.add('x');
  s.remove('x');
  assert.ok(!s.has('x'));
  s.add('x');                 // a fresh tag, not covered by the earlier remove
  assert.ok(s.has('x'));
});

test('concurrent add wins over remove', () => {
  // both replicas start knowing {x}
  const a = new ORSet('A');
  a.add('x');
  const b = a.clone();
  b.replicaId = 'B';

  a.remove('x');              // A removes the tag it has observed
  b.add('x');                 // B concurrently re-adds → new tag A never saw

  const merged = a.clone().merge(b);
  assert.ok(merged.has('x'), 'the concurrent add survives the remove (add-wins)');
  assert.ok(merged.equals(b.clone().merge(a)), 'converges regardless of merge order');
});

test('merge is idempotent and order-independent', () => {
  const a = new ORSet('A');
  const b = new ORSet('B');
  a.add('a').add('shared');
  b.add('b').add('shared');
  b.remove('shared');

  const ab = a.clone().merge(b);
  const ba = b.clone().merge(a);
  assert.ok(ab.equals(ba), 'commutative');
  assert.ok(ab.equals(ab.clone().merge(b).merge(a)), 'idempotent');
  // 'shared' was added by A and removed by B; A's add-tag survives → add wins
  assert.ok(ab.has('shared') && ab.has('a') && ab.has('b'));
});
