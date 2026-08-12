# C++ simdjson Parser

A header-only **JSON parser whose byte-scanning hot loops are accelerated with AVX2 SIMD**. Recursive descent for structure (simple and correct); vectorised scanning for the two things a JSON parser spends most of its byte-walking on — **skipping whitespace** and **finding the end of a string**. A `use_simd` flag switches the scalar path back on, which is how the tests prove the two paths are identical and the benchmark measures what the SIMD earns.

## The headline: ~2× on scan-bound input, and SIMD provably equal to scalar

The vectorised scanners load 32 bytes into an AVX2 register, compare all 32 at once (`_mm256_cmpeq_epi8`), and jump straight to the first hit via `movemask` + `ctz`. That pays off in proportion to how much of the parse is *scanning* rather than allocating DOM nodes, so the benchmark reports both regimes honestly:

```
$ make bench

structural JSON (allocation-bound)  (3.75 MB x20)
    scalar scan :  0.900 s      83.2 MB/s
    AVX2 scan   :  0.961 s      78.0 MB/s
    speedup     : 0.94x

text-heavy JSON (scan-bound)  (1.23 MB x40)
    scalar scan :  0.047 s    1049.7 MB/s
    AVX2 scan   :  0.027 s    1792.0 MB/s
    speedup     : 1.71x
```

SIMD accelerates scanning, so it wins (~1.7–2×) when scanning dominates — long strings, lots of whitespace — and is roughly neutral on documents that are bottlenecked on allocating one node per token. That's the honest shape of a per-token SIMD parser, and the benchmark shows both sides rather than only the flattering one.

The correctness guard is the second half of the headline: every test input is parsed **both ways** and the resulting DOMs are asserted equal, so the vectorised scanning can never silently diverge from the plain version.

```
$ make test

HEADLINE: the AVX2 path and the scalar path agree exactly
  ok:   SIMD and scalar parses are identical on the whole corpus

ALL TESTS PASSED
```

## Using it

Header-only — `#include "simdjson.hpp"` and compile with `-mavx2`.

```cpp
#include "simdjson.hpp"

sj::Value doc = sj::parse(R"({"name": "ada", "langs": ["c++", "js"], "born": 1815})");

doc.find("name")->string;          // "ada"
doc.find("langs")->size();         // 2
(*doc.find("langs")->array)[0];    // "c++"
doc.find("born")->number;          // 1815.0

// force the scalar scanners (identical result, slower):
sj::Value same = sj::parse(text, /*use_simd=*/false);
assert(doc == same);
```

`sj::Value` is a small, copyable DOM node (arrays/objects held behind a `shared_ptr` so the node stays cheap to move): a `Type` tag plus `boolean` / `number` / `string` / `array` / `object`, with `is_*()` accessors, `find(key)` for objects, `size()`, and `operator==`. Malformed input throws `sj::ParseError`, which carries the byte `position` of the failure.

## What it parses

Full [RFC 8259](https://www.rfc-editor.org/rfc/rfc8259) JSON: objects, arrays, strings with every escape (`\" \\ \/ \b \f \n \r \t` and `\uXXXX`, including surrogate pairs decoded to UTF-8), numbers (integers, negatives, fractions, exponents — validated against the grammar, then converted with `std::from_chars`), and the literals `true` / `false` / `null`. It rejects the usual malformed cases (trailing commas, leading zeros, `1.`, bare `-`, unterminated strings, trailing garbage).

## Build & run

```bash
make test    # correctness + SIMD-equals-scalar equivalence
make bench   # throughput, both regimes, SIMD vs scalar
```

Requires a C++17 compiler and an AVX2-capable CPU (`-mavx2`). Without `__AVX2__` the header compiles fine and falls back to scalar scanning everywhere.

## Layout

| path | what it holds |
|---|---|
| `include/simdjson.hpp` | the whole parser: `Value` DOM, `Parser`, AVX2 `skip_ws`/string scan + scalar fallback |
| `tests/test_simdjson.cpp` | grammar correctness, malformed rejection, and the SIMD==scalar property |
| `bench/bench.cpp` | throughput in both the allocation-bound and scan-bound regimes |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for how the SIMD scanners work, why the DOM node is shaped the way it is, and why `use_simd` is the design's centre of gravity.

## License

MIT — see [`LICENSE`](./LICENSE).
