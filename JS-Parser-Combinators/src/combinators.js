// A parser-combinator library.
//
// A *parser* is just a function from a parsing state `{ input, pos }` to a
// result — either a success `{ ok:true, value, pos }` (with the new position)
// or a failure `{ ok:false, pos, expected }`. Because parsers are plain values,
// you build big ones by *combining* small ones: `seq`, `alt`, `many`, `map`, …
// are higher-order functions that take parsers and return parsers. A grammar
// becomes an expression, not a hand-written state machine — and it reads like
// the grammar it implements (see `json.js` and `arith.js`).

const success = (value, pos) => ({ ok: true, value, pos });
const failure = (pos, expected) => ({ ok: false, pos, expected });

// -------------------------------------------------------------- primitives

/** Match a literal string. */
export const str = (s) => (st) =>
  st.input.startsWith(s, st.pos) ? success(s, st.pos + s.length) : failure(st.pos, JSON.stringify(s));

/** Match a regular expression anchored at the current position. */
export const regex = (re, label) => {
  const sticky = new RegExp(re.source, re.flags.includes('y') ? re.flags : re.flags + 'y');
  return (st) => {
    sticky.lastIndex = st.pos;
    const m = sticky.exec(st.input);
    return m ? success(m[0], st.pos + m[0].length) : failure(st.pos, label ?? re.toString());
  };
};

/** Match one character satisfying `pred`. */
export const satisfy = (pred, label = 'character') => (st) => {
  const c = st.input[st.pos];
  return c !== undefined && pred(c) ? success(c, st.pos + 1) : failure(st.pos, label);
};

/** Succeeds only at end of input. */
export const eof = (st) =>
  st.pos >= st.input.length ? success(null, st.pos) : failure(st.pos, 'end of input');

// -------------------------------------------------------------- combinators

/** Run parsers in order; yield the array of their values. */
export const seq = (...ps) => (st) => {
  const out = [];
  let pos = st.pos;
  for (const p of ps) {
    const r = p({ input: st.input, pos });
    if (!r.ok) return r;
    out.push(r.value);
    pos = r.pos;
  }
  return success(out, pos);
};

/** Ordered choice: the first parser that succeeds. On total failure, report the
 *  error that got *furthest* — usually the most informative one. */
export const alt = (...ps) => (st) => {
  let best = null;
  for (const p of ps) {
    const r = p(st);
    if (r.ok) return r;
    if (!best || r.pos > best.pos) best = r;
  }
  return best ?? failure(st.pos, 'no alternative');
};

/** Transform the value of a successful parse. */
export const map = (p, f) => (st) => {
  const r = p(st);
  return r.ok ? success(f(r.value), r.pos) : r;
};

/** Zero or more. Stops if the inner parser ever succeeds without consuming input
 *  (which would otherwise loop forever). */
export const many = (p) => (st) => {
  const out = [];
  let pos = st.pos;
  for (;;) {
    const r = p({ input: st.input, pos });
    if (!r.ok) break;
    if (r.pos === pos) break;             // no progress → stop
    out.push(r.value);
    pos = r.pos;
  }
  return success(out, pos);
};

/** One or more. */
export const many1 = (p) => map(seq(p, many(p)), ([first, rest]) => [first, ...rest]);

/** Zero or one; yields `dflt` when absent. */
export const optional = (p, dflt = null) => alt(p, (st) => success(dflt, st.pos));

/** `open` p `close`, yielding p's value. */
export const between = (open, p, close) => map(seq(open, p, close), ([, v]) => v);

/** Zero or more `p` separated by `sep`. */
export const sepBy = (p, sep) => optional(sepBy1(p, sep), []);

/** One or more `p` separated by `sep`. */
export const sepBy1 = (p, sep) =>
  map(seq(p, many(map(seq(sep, p), ([, v]) => v))), ([first, rest]) => [first, ...rest]);

/** Defer construction, so a grammar can refer to itself (recursion). */
export const lazy = (make) => (st) => make()(st);

/** Replace a parser's failure label with a friendlier name. */
export const label = (p, name) => (st) => {
  const r = p(st);
  return r.ok ? r : failure(r.pos, name);
};

/** Left-associative chain: `p (op p)*`, folding with the function `op` yields. */
export const chainl1 = (p, op) => (st) => {
  let r = p(st);
  if (!r.ok) return r;
  let { value, pos } = r;
  for (;;) {
    const o = op({ input: st.input, pos });
    if (!o.ok) break;
    const rhs = p({ input: st.input, pos: o.pos });
    if (!rhs.ok) return rhs;
    value = o.value(value, rhs.value);
    pos = rhs.pos;
  }
  return success(value, pos);
};

// -------------------------------------------------------------- running

export class ParseError extends Error {}

/** Run a parser over `input`, requiring it to consume all of it. Returns the
 *  value, or throws a `ParseError` pointing at the line/column of the failure. */
export function parse(parser, input) {
  const r = seq(parser, eof)({ input, pos: 0 });
  if (r.ok) return r.value[0];
  throw new ParseError(formatError(input, r.pos, r.expected));
}

function formatError(input, pos, expected) {
  let line = 1, col = 1;
  for (let i = 0; i < pos && i < input.length; i++) {
    if (input[i] === '\n') { line++; col = 1; } else col++;
  }
  const got = pos < input.length ? JSON.stringify(input[pos]) : 'end of input';
  return `parse error at line ${line}, column ${col}: expected ${expected}, got ${got}`;
}
