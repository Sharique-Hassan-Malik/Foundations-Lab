import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ParseError } from '../src/combinators.js';
import { parseJSON } from '../src/json.js';

test('agrees with JSON.parse on a corpus of documents', () => {
  const corpus = [
    '123', '-0.5', '0', '1e10', '3.14E-2', '-2.5e+3',
    '"hello"', '"with \\"quotes\\""', '"line\\nbreak\\ttab"', '"unicode \\u0041\\u00e9"', '"slash \\/"',
    'true', 'false', 'null',
    '[]', '[1, 2, 3]', '[1, [2, [3, []]]]', '[true, false, null, "x", 1.5]',
    '{}', '{"a": 1}', '{"a": 1, "b": [true, null], "c": {"d": "e"}}',
    '   {  "spaced" :\n  [ 1 , 2 ]  }   ',
    '{"": "empty key", "nested": {"deep": [1, {"x": [2, 3]}]}}',
  ];
  for (const doc of corpus) {
    assert.deepStrictEqual(parseJSON(doc), JSON.parse(doc), `mismatch for: ${doc}`);
  }
});

test('HEADLINE: matches JSON.parse on every short escape/unicode string (values AND keys)', () => {
  // exhaustively enumerate strings over the hard characters — quotes,
  // backslashes, control chars, a slash, unicode — up to length 3, and check the
  // parser agrees with JSON.parse both when the string is a value and when it is
  // an object key. This is the escaping corner where hand-rolled parsers break.
  const alpha = ['"', '\\', '\t', '\n', '/', 'a', '漢', 'é'];
  let count = 0;
  const each = (prefix, len, cb) => {
    if (len === 0) { cb(prefix); return; }
    for (const c of alpha) each(prefix + c, len - 1, cb);
  };
  for (let L = 0; L <= 3; L++) {
    each('', L, (s) => {
      const asValue = JSON.stringify(s);
      assert.deepStrictEqual(parseJSON(asValue), JSON.parse(asValue), `value ${asValue}`);
      const asKey = JSON.stringify({ [s]: [s, 1, true] });
      assert.deepStrictEqual(parseJSON(asKey), JSON.parse(asKey), `key ${asKey}`);
      count += 2;
    });
  }
  assert.ok(count > 1000, `checked ${count} cases`);
});

test('matches JSON.parse on random nested documents', () => {
  const rng = mulberry32(20260806);
  const word = () => 'k' + Math.floor(rng() * 1000);   // simple content: nesting is the target
  function randomValue(depth) {
    const kinds = depth <= 0 ? 4 : 6;
    switch (Math.floor(rng() * kinds)) {
      case 0: return Math.floor((rng() - 0.5) * 1e6);
      case 1: return Number(((rng() - 0.5) * 1000).toFixed(4)) || 0;   // normalise -0 → 0
      case 2: return word();
      case 3: return rng() < 0.33 ? true : rng() < 0.5 ? false : null;
      case 4: { const n = Math.floor(rng() * 5); return Array.from({ length: n }, () => randomValue(depth - 1)); }
      default: { const o = {}; const n = Math.floor(rng() * 5);
                 for (let i = 0; i < n; i++) o[word()] = randomValue(depth - 1); return o; }
    }
  }
  for (let i = 0; i < 3000; i++) {
    const text = JSON.stringify(randomValue(4));
    assert.deepStrictEqual(parseJSON(text), JSON.parse(text), `failed for: ${text}`);
  }
});

test('rejects malformed JSON with a positioned error', () => {
  for (const bad of ['', '{', '[1,]', '{"a": }', '[1 2]', 'tru', '"unterminated', '{"a": 1,}']) {
    assert.throws(() => parseJSON(bad), ParseError, `should reject: ${bad}`);
  }
  try {
    parseJSON('[1, 2 3]');
  } catch (e) {
    assert.match(e.message, /line 1, column/);
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
