# Architecture

The whole library rests on one type: a **parser is a function** `(state) → result`, where `state = { input, pos }` and `result` is either `{ ok: true, value, pos }` (success, with the position advanced past what it consumed) or `{ ok: false, pos, expected }` (failure, with where and what was expected). Everything else is higher-order functions over that type.

## Threading the position

There is no mutable cursor and no shared scanner. Each parser receives a state and, on success, returns a *new* position; a combinator that runs several parsers threads that position from one to the next. `seq`, for instance, starts at `st.pos`, runs each child at the current position, and on success advances to the child's returned position — stopping and propagating the first failure. Because state is passed by value, backtracking is free: `alt` simply hands each alternative the *same* original state, so a failed alternative leaves nothing to undo.

## The core combinators

- **`seq(...ps)`** — run in order, collect the values into an array, fail on the first child that fails.
- **`alt(...ps)`** — ordered choice: return the first success. When every alternative fails it returns the failure that reached the *furthest* position, which is almost always the most useful error ("expected `]`" at the real problem, not "expected `{`" at the start).
- **`many(p)`** — apply `p` until it fails, collecting values. It carries one crucial guard: if `p` ever succeeds *without advancing the position* (e.g. a regex that can match empty), `many` stops instead of looping forever. `many1`, `sepBy`, `optional`, and `between` are thin wrappers over `seq`, `alt`, and `map`.
- **`map(p, f)`** — run `p` and transform its value, leaving the position logic untouched. This is where a matched string becomes a number, or a pair becomes an object entry.

## Recursion, via `lazy`

A grammar is self-referential — a JSON value can contain an array which contains values — but a plain `const value = alt(..., array, ...)` would evaluate `array`, which needs `value`, before `value` exists. `lazy(() => make())` defers building the parser until it is first run, breaking the initialization cycle while keeping the definitions readable and mutually recursive.

## Precedence and associativity, via `chainl1`

Operator precedence can't be expressed by a flat matcher; it comes from *layering* the grammar. The arithmetic evaluator has `expr` (`+`/`-`) built on `term` (`*`/`-`) built on `factor` (a number, a parenthesised `expr`, or a unary minus). Because `*`/`/` live one layer *below* `+`/`-`, they bind tighter automatically.

Within a layer, `chainl1(p, op)` parses `p (op p)*` and folds it **left-associatively**: it reads the first operand, then repeatedly reads an operator and the next operand, applying the operator's function to the accumulated result and the new operand — so `10 - 4 - 3` folds as `((10 - 4) - 3)`, the arithmetically correct answer. Each operator parser (via `map`) yields the actual binary function to apply, so the parse *is* the evaluation.

## Errors and running

`parse(p, input)` runs `seq(p, eof)` — the `eof` forces the parser to consume the *entire* input, so trailing garbage is an error rather than being silently ignored. On failure it converts the failure position into a line and column by counting newlines up to that point and reports the `expected` label alongside the character actually found, producing messages like `parse error at line 1, column 7: expected "]", got "3"`.

## Building JSON on top

`json.js` is the payoff: the JSON grammar transcribed almost verbatim. Whitespace handling is factored into a `tok` helper that consumes trailing spaces after each token; strings are `many` of (an escape sequence — including `\uXXXX` decoded with `String.fromCharCode` — or an ordinary character) between quotes; numbers are the JSON number regex passed through `Number`; and objects and arrays are `sepBy` of their elements between braces or brackets, tied together with `lazy`. It agrees with the platform `JSON.parse` — verified by exhaustively enumerating the tricky escape/unicode strings (as both values and keys), since faithful escape handling is exactly where a JSON parser earns its correctness.
