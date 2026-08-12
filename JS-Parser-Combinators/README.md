# js-parser-combinators

A **parser-combinator** library in JavaScript — the functional approach to parsing where you build a big parser by *combining* small ones. To prove it, two real parsers are assembled from it: a **JSON parser** that agrees with the platform `JSON.parse`, and an **arithmetic evaluator** with correct operator precedence and associativity.

## The idea

A parser is just a function from a parsing state `{ input, pos }` to a result — a success carrying a value and the new position, or a failure carrying a position and what was expected. Because parsers are ordinary values, the library is a handful of higher-order functions that take parsers and return parsers: `seq`, `alt`, `many`, `map`, `sepBy`, `between`, `chainl1`, `lazy`, `optional`. A grammar written with them reads like the grammar itself:

```js
import { seq, sepBy, between, map, str, regex } from './src/combinators.js';

const number = map(regex(/-?\d+/), Number);
const list = between(str('['), sepBy(number, str(',')), str(']'));
parse(list, '[1,2,3]');        // [1, 2, 3]
```

## The headline: a JSON parser that agrees with `JSON.parse`

`src/json.js` is a full JSON parser built entirely from these combinators — quoted strings with every escape (`\n`, `\t`, `\"`, `\\`, `\/`, `\uXXXX`), the JSON number grammar, nested arrays and objects, and whitespace. It is checked against the platform `JSON.parse`:

```
$ npm test

✔ agrees with JSON.parse on a corpus of documents
✔ HEADLINE: matches JSON.parse on every short escape/unicode string (values AND keys)
✔ matches JSON.parse on random nested documents
✔ rejects malformed JSON with a positioned error
…
ℹ tests 17   ℹ pass 17   ℹ fail 0
```

The headline test is exhaustive where it counts: it enumerates *every* string of length ≤ 3 over the hard characters — quotes, backslashes, tabs, newlines, slashes, and unicode (`漢`, `é`) — and checks the parser matches `JSON.parse` both when the string is a value and when it is an object key. Escaping is exactly where hand-written JSON parsers go wrong, so this is the corner the test hammers. A separate test throws 3,000 random nested documents at it, and a fourth checks that malformed input is rejected with a line/column.

## The arithmetic evaluator: precedence and associativity

`src/arith.js` shows what a flat regex can't do — operator precedence and associativity — in a few lines, by layering the grammar (`expr` over `term` over `factor`) and using `chainl1` for left-association:

```
$ npm run demo

Arithmetic evaluator (precedence + associativity):
  1 + 2 * 3        = 7
  (1 + 2) * 3      = 9
  10 - 4 - 3       = 3        (left-associative: (10-4)-3)
  -2 * (3 + 4)     = -14

Error reporting points at the problem:
  "[1, 2 3]" → parse error at line 1, column 7: expected "]", got "3"
  "1 + * 2"  → parse error at line 1, column 5: expected number, got "*"
```

A test checks it against JavaScript's own evaluation on 500 random expressions.

## The combinators

| combinator | what it does |
|---|---|
| `str` / `regex` / `satisfy` | primitive matchers (literal, regex, one predicate char) |
| `seq(...)` / `alt(...)` | sequence (collect values) / ordered choice (first success; furthest error on failure) |
| `many` / `many1` / `optional` | repetition; `many` refuses to loop on empty matches |
| `sepBy` / `sepBy1` / `between` | separated lists, delimited groups |
| `map` | transform a parsed value |
| `chainl1` | left-associative operator chains (precedence layers) |
| `lazy` | defer construction, so a grammar can refer to itself (recursion) |
| `parse` | run a parser, require full consumption, throw a positioned `ParseError` |

## Run it

```bash
npm test        # node --test: combinators, the JSON parser, the evaluator
npm run demo     # a quick tour
```

Requires Node 18+ (built-in `node:test`). **No dependencies.**

## Layout

| path | what it holds |
|---|---|
| `src/combinators.js` | the library: primitives, combinators, `parse` + positioned errors |
| `src/json.js` | a JSON parser assembled from the combinators |
| `src/arith.js` | an arithmetic evaluator (precedence, associativity, unary minus) |
| `tests/*.test.js` | combinator unit tests + the JSON and arithmetic suites |
| `demo/run.js` | the tour above |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for how the state-threading works, why `alt` reports the furthest error, how `chainl1` encodes associativity, and how `lazy` breaks the recursion cycle.

## License

MIT — see [`LICENSE`](./LICENSE).
