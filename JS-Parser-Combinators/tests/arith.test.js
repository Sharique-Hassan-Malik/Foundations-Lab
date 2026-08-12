import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ParseError } from '../src/combinators.js';
import { evalExpr } from '../src/arith.js';

test('operator precedence: * and / bind tighter than + and -', () => {
  assert.equal(evalExpr('1 + 2 * 3'), 7);
  assert.equal(evalExpr('2 * 3 + 4 * 5'), 26);
  assert.equal(evalExpr('1 + 6 / 2'), 4);
});

test('associativity is left-to-right', () => {
  assert.equal(evalExpr('1 - 2 - 3'), -4);       // (1-2)-3
  assert.equal(evalExpr('16 / 2 / 2'), 4);        // (16/2)/2
});

test('parentheses override precedence; unary minus works', () => {
  assert.equal(evalExpr('(1 + 2) * 3'), 9);
  assert.equal(evalExpr('-3 + 5'), 2);
  assert.equal(evalExpr('-(2 + 3)'), -5);
  assert.equal(evalExpr('2 * -3'), -6);
});

test('whitespace and decimals are handled', () => {
  assert.equal(evalExpr('  3.5   *   2 '), 7);
  assert.equal(evalExpr('10'), 10);
});

test('matches JS evaluation on 500 random expressions', () => {
  const rng = mulberry32(7);
  function gen(depth) {
    if (depth <= 0 || rng() < 0.4) return String(1 + Math.floor(rng() * 9));   // 1..9
    const ops = ['+', '-', '*'];                                               // exact-integer ops
    const op = ops[Math.floor(rng() * ops.length)];
    const e = `(${gen(depth - 1)} ${op} ${gen(depth - 1)})`;
    return e;
  }
  for (let i = 0; i < 500; i++) {
    const expr = gen(4);
    // reference: JS itself (safe — the string is only digits, + - * and parens)
    const expected = Function(`return (${expr});`)();
    assert.equal(evalExpr(expr), expected, `mismatch for ${expr}`);
  }
});

test('rejects malformed expressions', () => {
  for (const bad of ['1 +', '* 2', '(1 + 2', '1 2', '', '1 + + 2']) {
    assert.throws(() => evalExpr(bad), ParseError, `should reject: ${bad}`);
  }
});

function mulberry32(seed) {
  return function () {
    seed |= 0; seed = (seed + 0x6D2B79F5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
