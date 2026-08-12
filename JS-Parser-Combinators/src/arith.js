// An arithmetic expression evaluator built from the combinators, demonstrating
// operator *precedence* and *associativity* — the things a flat regex or split
// can't express. The grammar is layered so that `*` and `/` bind tighter than
// `+` and `-`, and `chainl1` makes each level left-associative, so `1 - 2 - 3`
// is `(1-2)-3 = -4`, not `1-(2-3) = 2`.

import { alt, between, chainl1, lazy, map, parse, regex, seq, str } from './combinators.js';

const ws  = regex(/[ \t]*/);
const tok = (p) => map(seq(p, ws), ([v]) => v);

const number = map(tok(regex(/\d+(?:\.\d+)?/, 'number')), Number);
const lparen = tok(str('('));
const rparen = tok(str(')'));

// precedence, low to high:  expr( + - )  →  term( * / )  →  factor
const expr = lazy(() => chainl1(term, addOp));
const term = lazy(() => chainl1(factor, mulOp));
const factor = lazy(() => alt(
  number,
  between(lparen, expr, rparen),
  map(seq(tok(str('-')), factor), ([, v]) => -v),   // unary minus
));

const addOp = alt(
  map(tok(str('+')), () => (a, b) => a + b),
  map(tok(str('-')), () => (a, b) => a - b),
);
const mulOp = alt(
  map(tok(str('*')), () => (a, b) => a * b),
  map(tok(str('/')), () => (a, b) => a / b),
);

const program = map(seq(ws, expr), ([, v]) => v);

/** Evaluate an arithmetic expression string to a number. */
export function evalExpr(text) {
  return parse(program, text);
}
