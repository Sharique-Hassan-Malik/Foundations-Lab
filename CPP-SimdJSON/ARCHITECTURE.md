# Architecture

The parser is deliberately split into two layers with very different characters:

* **structure** is handled by ordinary recursive descent — one small function per
  grammar production (`parse_value`, `parse_object`, `parse_array`, `parse_string`,
  `parse_number`). This is the part that has to be *correct*, so it stays boring.

* **scanning** — walking runs of bytes to find where the next interesting thing is —
  is handled by two functions (`skip_ws` and `scan_to_string_special`) that have an
  AVX2 fast path. This is the part that dominates the byte count, so it's the part
  worth vectorising.

Keeping those separate is the whole idea: the SIMD lives in two leaf functions, and
everything above them is unaware it exists.

## The two SIMD scanners

Both follow the same shape, which is the standard SIMD "find first byte matching a
set" idiom:

1. Load 32 bytes with `_mm256_loadu_si256` (unaligned — the input isn't aligned).
2. Compare all 32 lanes at once against each character of interest with
   `_mm256_cmpeq_epi8`, OR the results together.
3. `_mm256_movemask_epi8` collapses the 32 lane results into a 32-bit integer, one
   bit per byte.
4. If that integer is non-zero, `__builtin_ctz` gives the index of the first hit, and
   we advance straight to it. Otherwise the whole 32-byte block is uninteresting, so
   advance 32 and loop.

`skip_ws` looks for the first *non*-whitespace byte, so it inverts the mask (`~`) —
whitespace is space/tab/newline/carriage-return. `scan_to_string_special` looks for
the first `"` or `\` inside a string body: `"` ends the string, `\` starts an escape.
The scalar tail (`while (p_ < end_ && …) ++p_;`) handles the final <32 bytes and is
also the entire implementation when SIMD is disabled or unavailable.

Note that `scan_to_string_special` finds the end of an ordinary run; `parse_string`
then copies that whole run in one `std::string::append`, so a long string body costs
one vectorised scan plus one `memcpy`, not a per-character loop.

## Why a `use_simd` flag is the centre of the design

The risk with hand-written SIMD is that it silently disagrees with the obvious scalar
version on some edge case — a byte near a 32-byte boundary, a control character, a
backslash right before the close quote. The defence is built into the parser: the
same `Parser` runs either path depending on a `bool simd_`, and the two paths share
*everything* except the scan loops. That makes an exact-equivalence test trivial to
write and impossible to fake — parse each input twice and compare the DOMs:

```cpp
sj::parse(doc, /*use_simd=*/true) == sj::parse(doc, /*use_simd=*/false)
```

The test corpus deliberately includes strings and whitespace runs longer than 32
bytes so the SIMD path actually takes its vectorised branch across chunk boundaries,
which is exactly where a boundary bug would hide. The same flag is what lets the
benchmark attribute a speedup to the SIMD alone: both timings run the identical
parser, so the only variable is the scanner.

## The DOM node

`Value` is a tagged union-by-convention: a `Type` enum and one field per possible
payload. Arrays and objects are held behind a `std::shared_ptr`, which keeps `Value`
itself small and cheap to copy and move (an important property when it's returned by
value all the way up the recursion and pushed into vectors) and gives value semantics
for free — copying a document shares the substructure rather than deep-copying it.

`Object` is a `vector<pair<string,Value>>`, not a hash map. JSON objects are usually
small, insertion order is worth preserving, and the spec permits (and real data
contains) duplicate keys — a `vector` handles all three, and `find` is a linear scan
that's faster than hashing for the sizes that actually occur.

`operator==` compares structurally (recursing into arrays and objects), which is what
makes the SIMD-equals-scalar assertion a one-liner.

## Numbers, strings, and errors

Numbers are first validated by walking the JSON number grammar by hand (optional
sign, integer part with no leading zeros, optional fraction, optional exponent) and
then converted with `std::from_chars`, which is locale-independent and allocation-free.
Validating first means malformed numbers are rejected with a position rather than
silently truncated by the conversion.

String escapes are expanded inline; `\uXXXX` reads four hex digits, and a
high-surrogate (`\uD800`–`\uDBFF`) immediately followed by a low-surrogate is combined
into the astral code point before being encoded to UTF-8 (so `😀` becomes the
four bytes of 😀). Every failure path throws `ParseError` carrying the byte offset, so
callers can point at exactly where the input went wrong.
