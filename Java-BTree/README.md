# Java B+ Tree

An in-memory **B+ tree** in Java — the balanced, high-fan-out search tree that backs the indexes of almost every relational database (and the interior of a great many file systems). Ordered key/value storage with `O(log n)` lookup, insert and delete, plus fast ordered range scans along a chained leaf level. Built from scratch, including the fiddly parts: node splitting on insert, and borrow-or-merge rebalancing on delete.

## The headline: it stays a correct, balanced B+ tree

A B-tree is only useful if it stays *balanced* — every leaf at the same depth — through arbitrary traffic. That is verified, not assumed: across 40,000 random insert / overwrite / delete operations at several branching factors, the tree is checked after every batch against every B+ invariant, and every lookup and ordered scan is checked against an in-memory `TreeMap`.

```
$ make test

randomised: matches TreeMap, and stays a valid balanced B+ tree
  ok:   order 3: invariants held throughout (null)
  ok:   order 3: every lookup matches TreeMap
  ok:   order 3: full ordered scan matches TreeMap
  ok:   order 32: invariants held throughout (null)
  …
  ok:   remove() return values matched TreeMap throughout
  ok:   200 random range scans match TreeMap.subMap

height stays logarithmic (the whole point of a B-tree)
  ok:   1,000,000 keys fit in height 4 (≤ 5)
  ok:   1M-key tree is a valid B+ tree

ALL TESTS PASSED
```

The invariant check enforces the properties that *make* it a B+ tree: **all leaves at the same depth**; every non-root node between `⌈order/2⌉−1` and `order−1` keys (no under- or over-full nodes); keys sorted within every node and bounded by their ancestors' separators; and a leaf chain that is globally sorted and whose length equals the tree's size.

## Performance

```
$ make bench     # order 128

  insert: 2,000,000 keys in 1.55 s = 1,292,390 puts/s (height 4)
  lookup: 2,000,000 random gets in 3.13 s =   639,318 gets/s
```

Two million keys sit in a tree just **four levels deep** — the high fan-out (up to 127 keys per node) is what keeps the height, and therefore the number of comparisons per lookup, tiny.

## Using it

```java
BPlusTree<Integer, String> t = new BPlusTree<>(64);   // order = max children per node
t.put(1, "a");
t.put(2, "b");
t.get(1);                       // "a"
t.remove(2);                    // "b" (the removed value)
for (var e : t.range(0, 10))    // ordered scan of [0, 10)
    System.out.println(e.key + " = " + e.value);
```

## Build & run

```bash
make test      # compile + the full test suite (reference fuzz, invariants, ranges, balance)
make bench      # insert/lookup throughput and the resulting height
make clean
```

Needs a JDK 17+ (`java`/`javac`); no dependencies.

## Layout

| path | what it holds |
|---|---|
| `src/btree/BPlusTree.java` | the tree: search, insert+split, delete+borrow/merge, range scans, `checkInvariants` |
| `test/btree/TestBTree.java` | basics, the TreeMap fuzz test with per-batch invariant audits, range scans, balance |
| `bench/btree/Bench.java` | insert and lookup throughput |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the node layout, exactly how a split and a merge move a separator up or down, and every invariant the checker enforces.

## License

MIT — see [`LICENSE`](./LICENSE).
