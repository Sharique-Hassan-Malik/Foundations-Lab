# Architecture

Four source files, split along the line the format itself draws: what the
encoder is free to choose, and what the decoder must be told.

```
    input ──▶ lz77_parse ──▶ tokens ──▶ count_symbols ──▶ Huffman tables
                (free)                                          │
                                                                ▼
                                              block type choice, then emit
```

`lz77_parse` can be replaced entirely — different hash function, different
chain depth, optimal parsing — without the decoder changing by a line. Which
matches to emit is an encoder's private business; how they are *coded* is the
format. Keeping the two in separate functions is what makes that true in the
code and not just in principle.

---

## The bit order, stated once

Two orders coexist in DEFLATE and mixing them up is the defining bug of a
from-scratch implementation, because it is invisible to your own tests:

- the stream is packed **LSB-first**;
- a Huffman code is written **MSB-first within it**;
- every other field is LSB-first.

So exactly one operation reverses, and it lives in exactly one function:
[`bw_code`](src/bits.c). The tables in `bits.c` are transcribed from RFC 1951
§3.2.5 unmodified, so they can be compared against the document line by line.
Pre-reversing them would make that impossible and would spread the reversal
across the encoder.

The decoder does not need a matching reversal. `huff_decode` walks bits from the
LSB end and accumulates them into a code most-significant-bit-first, which is the
same operation seen from the other side.

**Why this needs an external judge.** An encoder and decoder that both reverse,
or both do not, round-trip perfectly. Every self-test passes. The stream is not
DEFLATE. `tools/gzcheck.sh` exists for exactly this failure and nothing else:
`gunzip` has never seen this code and cannot share its mistakes.

## Canonical Huffman, and why no tree is transmitted

RFC 1951 §3.2.2 fixes the mapping from bit lengths to codes: within a length,
codes are assigned in increasing symbol order; lengths are processed shortest
first. So a code is fully described by one length per symbol, and that is all
that goes in the header.

`huff_assign_codes` is that rule. `huff_build_decoder` and `huff_decode` are the
same rule inverted. Neither side shares a table with the other, which means an
encoder and decoder written independently from the same paragraph agree — and it
means a bug in one of them cannot be hidden by a matching bug in the other.

### Length limiting

Codes may not exceed 15 bits (7 for the code-length alphabet). An unconstrained
Huffman tree over skewed frequencies can go deeper.

Package-merge solves this optimally. This uses the cheaper standard alternative:
build unconstrained, and if the depth exceeds the limit, halve every frequency
and rebuild. Halving compresses the ratio between the largest and smallest
frequency, and Huffman depth is bounded by that ratio, so it terminates.

The cost is a fraction of a percent of ratio, paid only on inputs that would
have exceeded the limit anyway. It is written down in the README as a
difference from zlib rather than left as a silent approximation.

## Why the block type is measured, not guessed

Three block types are legal: stored, fixed Huffman, dynamic Huffman. The
encoder builds the token stream once and then *costs all three in bits* before
emitting anything.

This is not an optimisation. It is the only way to guarantee the property a
compressor must have: **never make the input bigger**. Random data has no
exploitable structure, so a dynamic block spends a header describing a code that
saves nothing, and a fixed block spends 8-9 bits on every byte. Only the stored
block breaks even. An encoder that always emitted Huffman codes would expand
incompressible input — and the test that catches it is the one asserting random
bytes grow by no more than 0.1%.

The measured result: 200 KB of random data comes out 38 bytes larger, all of it
gzip and block headers.

## Lazy matching

Greedy LZ77 takes the longest match at each position. That is not optimal: a
4-byte match at position *i* may be preventing a 6-byte match at *i+1*, and
emitting one literal to reach it wins.

So levels 4 and above hold a found match back for one byte, look again, and keep
whichever is longer. The held match is committed as a literal plus the later
match if the later one wins.

The hash chain is updated at *every* position regardless of which branch wins —
including the interior bytes of a committed match. Skipping those is a tempting
saving and it silently degrades the ratio for the rest of the stream, because
future positions can no longer find matches that start inside this one.

## Where the decoder is strict

A decoder is the part that reads hostile input, so three checks are not
optional:

- **A distance may not reach before the start of the output.** Without this
  check a crafted stream reads memory before the buffer. It is one comparison.
- **An over-subscribed Huffman code is rejected** at table-build time, before
  any symbol is decoded. An *incomplete* code is accepted, because a block with
  one distance symbol legitimately produces a 1-bit code with an unused branch,
  and refusing those would reject streams zlib emits.
- **`NLEN` must complement `LEN`** in a stored block. It is the format's own
  redundancy check and skipping it discards free error detection.

Overlapping matches are copied **one byte at a time**, deliberately. A match of
length 20 at distance 1 repeats a single byte twenty times, which is legal and
common in run-heavy data. `memcpy` would read bytes it has not written yet.

## Errors are values

Every entry point returns a negative `DF_ERR_` code rather than aborting. The
bit reader is the one place that does not check per-operation: it feeds zeros
past the end and latches an `underflow` flag, checked once per block instead of
on every bit. Putting a branch on the hottest path in the decoder to report an
error one loop iteration earlier is not a trade worth making — but leaving the
condition undetected would be, which is why the flag exists at all.
