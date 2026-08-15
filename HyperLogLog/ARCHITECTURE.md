# Architecture

## The estimator, from the top

```
   item ──hash──▶ 64-bit value
                   ├── top p bits ─────────▶ which register (0 .. 2^p-1)
                   └── remaining 64-p bits ─▶ leading-zero run + 1 = "rank"
                                              register = max(register, rank)

   estimate():  harmonic mean of 2^register over all registers,
                scaled by α·m², with a linear-counting correction when sparse

   merge(other):  register[i] = max(register[i], other.register[i])
```

Everything is one `byte[]` of `2^p` registers and the four operations over it.
There is no per-item storage, which is the entire point: the memory is chosen up
front and never grows.

## Why leading zeros

Hash values are uniform, so a hash has probability `2^-k` of starting with `k`
zeros. Across `n` distinct items the longest run seen is around `log2(n)` — one
item in `n` is expected to have a run of `log2(n)` zeros. A single longest-run
observation is a terrible estimator (its variance is enormous), so HyperLogLog
runs `m` of them in parallel: the top `p` hash bits deposit each item into one of
`m = 2^p` registers, each of which tracks the longest run it has seen. Averaging
`m` noisy estimators cuts the relative error by `√m`, which is where the
`1.04 / √m` bound comes from.

Crucially, a register keeps only the **maximum** rank. Adding the same item again
recomputes the same hash, the same register, the same rank — and `max` leaves it
unchanged. That idempotence is why the structure counts *distinct* items and not
occurrences, and `testIdempotentOnDuplicates` pins it.

## The rank computation

```java
int index = (int) (hash >>> (64 - precision));   // top p bits
long remaining = hash << precision;              // the rest, in the high bits
int rank = remaining == 0 ? (64 - precision + 1)
                          : Long.numberOfLeadingZeros(remaining) + 1;
```

Shifting the remaining bits to the top of a `long` means `numberOfLeadingZeros`
counts the run directly — a single hardware instruction. The `remaining == 0`
case (all remaining bits zero) is the maximum possible rank, `64 - p + 1`, and
must be special-cased because `numberOfLeadingZeros(0)` is 64, which would be
wrong.

## The estimator and its correction

The raw estimate is the bias-corrected harmonic mean:

```
E = α_m · m² / Σ 2^(-register[i])
```

`α_m` is a constant (≈ 0.7213 for large `m`) that removes the systematic bias of
the harmonic-mean formula; the special values for `m ∈ {16, 32, 64}` are from the
original paper.

The harmonic mean is unreliable when the sketch is **sparse** — when most
registers are still zero, which happens when the true cardinality is small
relative to `m`. There, the method switches to **linear counting**, which
estimates cardinality from the fraction of empty registers:

```
if E ≤ 2.5m and some registers are zero:
    E = m · ln(m / emptyRegisters)
```

This is why `testSmallCountsAreNearlyExact` sees 100 distinct items estimated as
100 ± 2: in the small regime the estimator is nearly exact, not merely within its
asymptotic error bound.

The original large-range correction (for cardinalities near `2^32`, where 32-bit
hash collisions matter) is omitted deliberately: a **64-bit** hash pushes that
regime past billions of items, so it never triggers in practice.

## Hashing is load-bearing

The entire error analysis assumes hashes are uniform and independent. A weak hash
breaks the assumption and the bound with it. So even though the inputs in the
benchmark are `i * STRIDE`, the hash puts them through SplitMix64's finalizer:

```java
z ^= z >>> 30; z *= 0xBF58476D1CE4E5B9L;
z ^= z >>> 27; z *= 0x94D049BB133111EBL;
z ^= z >>> 31;
```

SplitMix64 is a counter-based generator precisely designed so that consecutive
integers produce independent-looking outputs — which is why
`testSequentialIntegersAreScrambled` can feed a dense `0..N-1` range, the worst
case for a naive hash, and still get a sane estimate. Byte and string inputs are
folded with FNV-1a first, then finalized.

## Mergeability, the design centre

`merge` is a register-wise `max`, and that one line is why the sketch is worth
using over exact counting in any distributed setting.

The maximum zero-run seen for a bucket across two streams' union is the maximum
of the two per-stream maxima — the `max` of maxes is the max of the union. So
after merging, `estimate()` returns the cardinality of `A ∪ B`, with the overlap
counted once, and it does so **without either shard's elements ever being
shipped**. The alternative, summing per-shard counts, double-counts the
intersection by exactly `|A ∩ B|`, an error precision cannot touch because the
intersection information was discarded when each shard became a number. The
benchmark's 0.46%-vs-19.91% split is that difference.

`union(a, b)` is commutative and merging a sketch with an identical one is the
identity, both because `max` is — `testMergeIsCommutative` and
`testMergingIdenticalSketchesIsIdentity` hold the design to it.

## Complexity

| operation | cost |
|---|---|
| `add` | `O(1)` — one hash, one register update |
| `estimate` | `O(m)` — one pass over the registers |
| `merge` | `O(m)` |
| memory | `m = 2^p` bytes, independent of cardinality |

## Files

| file | role |
|---|---|
| `src/hll/HyperLogLog.java` | the sketch and its hashing |
| `test/hll/HllTest.java` | 16 tests, zero-dependency runner |
| `bench/Benchmark.java` | the four measured results |

## What is deliberately absent

- **HyperLogLog++.** Google's refinements — a sparse representation for very low
  cardinalities and empirically-tabulated bias correction in the mid-range —
  lower the error further at the cost of a great deal of complexity and a large
  bias-correction table. The classic estimator keeps the idea legible and its
  error already matches theory.
- **Intersection via inclusion-exclusion.** `|A ∩ B| = |A| + |B| - |A ∪ B|` is
  computable, but subtracting two noisy estimates gives a result whose *relative*
  error explodes when the intersection is small. Offering it would be offering a
  footgun.
- **Serialization.** The state is a `byte[]`; writing it to bytes is a
  formatting concern, not an algorithmic one.
