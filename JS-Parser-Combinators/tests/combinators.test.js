import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  alt, between, chainl1, many, map, optional, parse, ParseError,
  regex, satisfy, sepBy, seq, str,
} from '../src/combinators.js';

test('str matches a literal and reports progress', () => {
  assert.equal(parse(str('hello'), 'hello'), 'hello');
  assert.throws(() => parse(str('hello'), 'help'), ParseError);
});

test('seq collects values; alt takes the first success', () => {
  const ab = seq(str('a'), str('b'));
  assert.deepEqual(parse(ab, 'ab'), ['a', 'b']);
  const aOrB = alt(str('a'), str('b'));
  assert.equal(parse(aOrB, 'b'), 'b');
});

test('many stops cleanly and never loops on empty matches', () => {
  const digits = many(satisfy((c) => c >= '0' && c <= '9'));
  assert.deepEqual(parse(seq(digits, str('!')), '123!'), [['1', '2', '3'], '!']);
  // regex that can match empty must not spin `many` forever
  const empties = many(regex(/a*/));
  assert.equal(parse(seq(empties, str('b')), 'b')[1], 'b');
});

test('sepBy handles zero, one, and many; between strips delimiters', () => {
  const list = between(str('['), sepBy(regex(/\d+/), str(',')), str(']'));
  assert.deepEqual(parse(list, '[]'), []);
  assert.deepEqual(parse(list, '[7]'), ['7']);
  assert.deepEqual(parse(list, '[1,2,3]'), ['1', '2', '3']);
});

test('optional yields a default; map transforms', () => {
  const signed = map(seq(optional(str('-'), ''), regex(/\d+/)), ([s, d]) => Number(s + d));
  assert.equal(parse(signed, '42'), 42);
  assert.equal(parse(signed, '-42'), -42);
});

test('chainl1 is left-associative', () => {
  const num = map(regex(/\d+/), Number);
  const minus = map(str('-'), () => (a, b) => a - b);
  // 9-4-2 must fold as (9-4)-2 = 3, not 9-(4-2) = 7
  assert.equal(parse(chainl1(num, minus), '9-4-2'), 3);
});

test('errors report line and column', () => {
  try {
    parse(seq(str('a'), str('b')), 'aX');
    assert.fail('should have thrown');
  } catch (e) {
    assert.ok(e instanceof ParseError);
    assert.match(e.message, /line 1, column 2/);
    assert.match(e.message, /got "X"/);
  }
});
