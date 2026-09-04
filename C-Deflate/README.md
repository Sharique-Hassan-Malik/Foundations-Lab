# DEFLATE From Scratch

LZ77 with hash-chain matching, canonical Huffman coding, and the three block
types of **RFC 1951** — written in C with no dependencies, and verified by
handing its output to a program that has never seen this code.

---

## The headline

```
$ make gzcheck

  PASS  gunzip -level 6- accepted text                      88000 -> 332      bytes
  PASS  gunzip -level 6- accepted source                    39324 -> 11490    bytes
  PASS  gunzip -level 6- accepted random                   200000 -> 200038   bytes
  ...
  Streams accepted : 28
  Streams rejected : 0
  GZIP ACCEPTANCE PASSED
```

**28 of 28 streams are accepted by `gunzip`** and decompress byte-identically —
seven input shapes across four compression levels. That is the claim worth
making, because it is the only one a self-test cannot fake. A round trip
through this project's own decoder proves the encoder and decoder agree with
each other, which they would even if both were wrong in the same way.

And the ratio stands up against the reference implementation:

| input | original | this, level 9 | `gzip -9` | difference |
|---|---|---|---|---|
| its own source | 39,324 | 11,370 | 11,346 | +0.2% |
| repeated text | 88,000 | 332 | 333 | −0.3% |
| 200 KB of `a` | 200,000 | 229 | 230 | −0.4% |

Within a fraction of a percent of zlib either way, from about 900 lines.

---

## Background

DEFLATE is two ideas stacked. **LZ77** replaces a repeated run of bytes with a
(length, distance) pair pointing backwards into what has already been decoded.
**Huffman coding** then gives the most common symbols the shortest bit codes.
The format is nearly thirty years old and is still what `gzip`, `zip`, `PNG`
and every HTTP `Content-Encoding: gzip` response use.

The interesting part is that almost none of the difficulty is in either
algorithm. It is in the encoding.

### The two bit orders

This is the detail that decides whether an implementation is real or merely
self-consistent. DEFLATE packs its stream **LSB-first** — bit 0 of a byte is
consumed first. But a Huffman code is written **MSB-first within that stream**:
the code's most significant bit occupies the earliest bit position.

Everything else — the extra bits after a length or distance code, `HLIT`,
`HDIST`, `HCLEN`, a stored block's `LEN` — is plain LSB-first.

So exactly one thing is reversed, and getting it wrong in *both* the encoder and
the decoder produces a codec that round-trips perfectly and that no other
program on earth can read. `make gzcheck` is what makes that failure impossible
to ship, and it is why the reversal lives in one function
([`bw_code`](src/bits.c)) rather than being baked into the tables, where it
would have to be undone to compare them against the RFC.

### Only lengths are transmitted

A Huffman code is sent as one bit *length* per symbol, never as a tree. RFC 1951
§3.2.2 fixes how lengths become codes: sort by (length, symbol) and assign
consecutively. Both sides run the same rule, so they agree by construction, and
the decoder is that rule read backwards.

The header carrying those lengths is itself Huffman-coded, with its own
19-symbol alphabet that has run-length symbols for repeats — a code, describing
a code, describing the data.

---

## Building

```bash
make test      # 17 self-checking tests, including 400 randomised round trips
make gzcheck   # hand the output to gunzip  (needs gzip installed)
make bench     # ratio and throughput
```

No dependencies beyond a C11 compiler. `gzcheck` shells out to `gunzip`, which
is the point of it.

## Using it

```c
#include "deflate.h"

uint8_t out[4096];
long n = deflate_compress(input, input_len, out, sizeof out, DF_LEVEL_DEFAULT);
if (n < 0) { /* DF_ERR_OUTPUT, DF_ERR_MEMORY … */ }

long back = deflate_decompress(out, (size_t)n, restored, sizeof restored);
```

`gzip_compress()` wraps the same stream in a gzip container so it can be written
straight to a `.gz` file.

Errors are returned, never signalled by crashing: a corrupt or truncated stream
produces a negative `DF_ERR_` value, because the caller is usually part-way
through a batch it still wants to finish.

---

## What the tests pin down

Round-trip on its own is a weak test, so the suite separates the properties:

- **Round trip is exact** across shapes chosen to reach each block type — empty
  input, five bytes, 100 KB of one byte, English text, random noise, and an
  overlapping match at distance 1.
- **Every match length from 3 to 258** round-trips, because the boundaries
  between length codes are where an off-by-one in the tables hides.
- **Compressible input actually compresses** (100 KB of `a` → 114 bytes). A
  codec that quietly stored everything would round-trip perfectly and fail here.
- **Incompressible input does not expand** by more than 0.1%. This is what
  proves the stored block exists *and* is chosen; an encoder that always emitted
  Huffman codes would make random data bigger, which is the one thing a
  compressor must never do.
- **Corrupt streams are refused** — a reserved block type, a stored block whose
  `NLEN` does not complement `LEN`, a truncated stream, and a distance pointing
  before the start of the output.
- **400 randomised inputs** round-trip, ~3.9 MB through the codec, from a fixed
  seed so a failure is reproducible.

---

## Performance

1 MiB of each, on one core:

```
corpus            level   compressed    ratio         MB/s
text-like             1         3645  287.68x        205.4
                      5         3644  287.75x        178.4
                      9         3644  287.75x        228.2
source-like           1        94461   11.10x         58.8
                      5        67992   15.42x         60.5
                      9        47710   21.98x         10.9
runs                  1        21798   48.10x        161.1
                      5        17205   60.95x        162.3
                      9        17463   60.05x         77.3
incompressible        9      1048661    1.00x         30.0

decompression (source-like): 295.4 MB/s
```

The level knob does what it claims: level 9 finds **twice** as much redundancy
in source-like data as level 1 (21.98x against 11.10x), and pays about 5× the
time for it.

---

## Non-goals, and one honest wart

- **One block per stream.** The encoder emits a single dynamic block for the
  whole input rather than splitting it where the statistics change. On a file
  that is half text and half binary, two blocks with their own Huffman tables
  would beat one block with a compromise table. Block splitting is a real
  ratio win and is not implemented.
- **Frequency halving, not package-merge.** Code lengths are limited to 15 bits
  by halving all frequencies and rebuilding when the limit is exceeded, rather
  than by the optimal package-merge algorithm. It costs a fraction of a percent
  on pathological inputs and saves a lot of code. Stated because it is a real,
  if small, difference from zlib.
- **Level 9 is not always better than level 5.** On the run-heavy corpus above
  it is 1.5% *worse* (17,463 against 17,205). A longer chain search finds longer
  individual matches, and lazy matching then sometimes takes a parse that is
  locally better and globally worse. Fixing that properly needs optimal parsing,
  which is a different project.
- **The whole input is buffered**, along with one token per literal or match.
  There is no streaming interface, so peak memory is roughly seven times the
  input size. Fine for the sizes this is for; not how you would compress a
  10 GB file.
- **No zlib (RFC 1950) wrapper**, only raw DEFLATE and gzip. Adding it is a
  two-byte header and an Adler-32, and it would not demonstrate anything the
  gzip path does not.

## Layout

```
include/deflate.h   the public API
src/internal.h      shared structures, and the bit-order rules stated once
src/bits.c          the two bit orders, and the RFC 1951 tables
src/huffman.c       length-limited code construction, canonical codes, decoding
src/deflate.c       hash-chain matching, lazy matching, block type choice
src/inflate.c       the decoder, CRC-32, and the gzip container
tests/              17 self-checking tests
tools/gzcheck.sh    hands the output to gunzip
bench/              ratio and throughput
```

See [ARCHITECTURE.md](./ARCHITECTURE.md) for the design decisions.

## License

MIT — see [`LICENSE`](./LICENSE).
