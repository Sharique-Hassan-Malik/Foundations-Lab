// A complete JSON parser, assembled entirely from the combinators. It reads
// almost exactly like the JSON grammar from RFC 8259, which is the point of the
// approach — and it agrees with the platform `JSON.parse` on the test corpus.

import {
  alt, between, lazy, many, map, parse, regex, satisfy, sepBy, seq, str,
} from './combinators.js';

const ws  = regex(/[ \t\n\r]*/, 'whitespace');
const tok = (p) => map(seq(p, ws), ([v]) => v);   // consume trailing whitespace
const sym = (s) => tok(str(s));

// literals
const jnull  = map(tok(str('null')),  () => null);
const jtrue  = map(tok(str('true')),  () => true);
const jfalse = map(tok(str('false')), () => false);

// numbers (the JSON number grammar)
const jnumber = map(
  tok(regex(/-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/, 'number')),
  Number,
);

// strings, with the full escape set incl. \uXXXX
const ESCAPES = { '"': '"', '\\': '\\', '/': '/', b: '\b', f: '\f', n: '\n', r: '\r', t: '\t' };
const hex4 = regex(/[0-9a-fA-F]{4}/, 'four hex digits');
const uEscape = map(seq(str('u'), hex4), ([, h]) => String.fromCharCode(parseInt(h, 16)));
const simpleEscape = map(satisfy((c) => c in ESCAPES, 'escape'), (c) => ESCAPES[c]);
const escapeSeq = map(seq(str('\\'), alt(uEscape, simpleEscape)), ([, v]) => v);
const strChar = alt(escapeSeq, satisfy((c) => c !== '"' && c !== '\\' && c >= ' ', 'string character'));
const jstring = tok(between(str('"'), map(many(strChar), (cs) => cs.join('')), str('"')));

// recursive structures
const jvalue = lazy(() => alt(jstring, jnumber, jobject, jarray, jtrue, jfalse, jnull));

const jarray = map(seq(sym('['), sepBy(jvalue, sym(',')), sym(']')), ([, items]) => items);

const jpair = map(seq(jstring, sym(':'), jvalue), ([k, , v]) => [k, v]);
const jobject = map(seq(sym('{'), sepBy(jpair, sym(',')), sym('}')),
                    ([, pairs]) => Object.fromEntries(pairs));

const document = map(seq(ws, jvalue), ([, v]) => v);

/** Parse a JSON document into the corresponding JS value. Throws a ParseError
 *  (with line/column) on malformed input. */
export function parseJSON(text) {
  return parse(document, text);
}
