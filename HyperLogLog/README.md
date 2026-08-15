# HyperLogLog

Count the number of **distinct** items in a stream to within about a percent,
using a fixed 16 KB — no matter whether the stream had a thousand items or ten
million. Written from scratch in Java, with the property that makes it
indispensable in distributed systems: two sketches **merge**.

---

## Background

Counting distinct values exactly means remembering every value you have seen. A
`HashSet<Long>` of ten million longs is hundreds of megabytes, and across a fleet
of servers you cannot even do it — you would have to ship every value to one
place to deduplicate.

HyperLogLog answers the same question from a bounded summary. Hash each item;
in a stream of many distinct items the rarest thing you expect to see is a hash
beginning with a long run of zeros, and the longest run is a (noisy) estimate of
`log2(distinct count)`. Split the stream into `m` buckets, track the longest run
in each, combine with a harmonic mean, and the noise averages down to a relative
error of about `1.04 / √m` — from `m` single-byte registers, **independent of the
count**.

---

## The headline

Same 16 KB sketch, four orders of magnitude of cardinality:

```
1. Accuracy at precision 14 (16384 registers = 16 KB), median of 5 trials:

  true count        estimate      error
------------  --------------  ---------
       1,000           1,002      0.50%
      10,000          10,006      0.49%
     100,000          99,077      0.32%
   1,000,000         990,387      0.21%
  10,000,000       9,992,057      0.40%
```

```
2. Memory: HyperLogLog vs an exact HashSet (measured retained heap)

1,000,000 distinct longs:
  HashSet<Long> : 65,585,376 bytes  (exact, 66 bytes/item)
  HyperLogLog   :     16,384 bytes  (est 995,410, error 0.46%)
  ratio         :      4,003x less memory
```

`./build.sh` (or `java -cp out Benchmark`)

The set grows with the data; the sketch does not. At ten million the set is
hundreds of megabytes and the sketch is still 16 KB.

## The error is a guarantee, not an average

Over 40 independent trials, the root-mean-square relative error lands on the
theoretical `1.04 / √m` at every precision:

```
precision        observed SE  theory 1.04/√m
------------  --------------  --------------
10                    3.557%          3.250%
12                    1.624%          1.625%
14                    0.777%          0.813%
```

You budget the memory (`m` bytes) and get a stated error bound in return — not a
figure that hides a bad tail. Doubling the precision halves the error and doubles
the memory, and you can pick the point on that curve you want.

## Mergeability — the reason it beats exact counting in a fleet

Two shards, each having seen 600,000 distinct items with a 200,000 overlap, so
the true union is exactly one million:

```
  true union            : 1,000,000
  merged sketches (max) :   995,410   (error 0.46%)
  naive sum of counts   : 1,199,121   (error 19.91%  — double-counts the overlap)
```

Adding per-shard counts overcounts by the size of the overlap — a 20% error that
**no amount of precision fixes**, because the information needed to subtract the
intersection was destroyed when each shard collapsed to a number. Merging the
sketches keeps it: the union's registers are the element-wise maximum of the
shards',

```java
for (int i = 0; i < m; i++)
    registers[i] = max(a.registers[i], b.registers[i]);
```

so the overlap is counted once, automatically, and **no element ever leaves its
shard**. Ten servers each keep a 16 KB sketch, the coordinator merges them, and
the answer is the fleet-wide distinct count. This is the same argument that makes
cumulative histograms, not pre-computed percentiles, the mergeable choice in
[Inference-Observability](https://github.com/Sharique-Hassan-Malik/Inference-Observability).

## Usage

```java
HyperLogLog hll = new HyperLogLog(14);   // 16 KB, ~0.8% error
hll.add(userId);
hll.add("some-string");
hll.add(byteArray);
long distinct = hll.estimate();

// merge across shards
HyperLogLog fleet = HyperLogLog.union(shardA, shardB);
long fleetDistinct = fleet.estimate();
```

```bash
./build.sh          # compile, test (22 assertions), benchmark — needs only a JDK
```

## What it does and does not do

**Does:** distinct-count estimation with a stated error bound, from fixed memory;
lossless merge (union); accurate small-cardinality counting via a linear-counting
correction; a strong 64-bit hash so even a dense `0..N-1` input is estimated
correctly.

**Does not:** the HyperLogLog++ refinements (a sparse representation for tiny
cardinalities, and empirical bias correction in the mid-range). They lower the
error a little more at low cardinalities and add substantial complexity; the
classic estimator with linear-counting is what makes the idea legible, and its
error already tracks theory. Intersection (`|A ∩ B|`) via inclusion-exclusion is
possible but its error is poor when the intersection is small — a real footgun,
left out on purpose.

## Layout

| file | role |
|---|---|
| `src/hll/HyperLogLog.java` | the sketch: add, estimate, merge, hashing |
| `test/hll/HllTest.java` | 16 tests + zero-dependency runner |
| `bench/Benchmark.java` | accuracy, memory, error-vs-theory, mergeability |

## License

MIT
