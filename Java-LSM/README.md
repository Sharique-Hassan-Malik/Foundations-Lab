# Java LSM-Tree Key-Value Store

A **log-structured merge-tree** key-value store in Java — the storage engine design underneath LevelDB, RocksDB, Cassandra, and HBase. Writes append to a durable log and a sorted in-memory table; when that fills it is flushed as an immutable **SSTable** on disk; reads consult the memtable and then each SSTable newest-to-oldest, skipping tables with a **Bloom filter** and seeking with a **sparse index**; **compaction** merges SSTables and reclaims space. Built from scratch — WAL, memtable, SSTable format, bloom filter, and compaction are all here.

## The headline: durability across a crash

Every write is appended to the write-ahead log and flushed to the OS *before* it is acknowledged, so a crash with an unflushed memtable loses nothing — reopening the tree replays the log. That is tested directly: write thousands of records (and deletes) with no flush, simulate a crash that drops the in-memory state, reopen, and check everything comes back.

```
$ make test

crash recovery (the headline): unflushed writes survive a crash
  ok:   nothing was flushed yet (0 SSTables)
  ok:   every un-flushed put recovered from the WAL
  ok:   every un-flushed delete recovered (keys stay gone)
```

The rest of the suite pins down correctness the hard way — against an in-memory reference map across 60,000 randomised operations *including* the flushes and compactions that happen mid-run:

```
randomised: matches an in-memory reference across flushes & compactions
  ok:   every point lookup matches the reference
  ok:   full ordered scan matches the reference, in key order
  ok:   range scan stays within [from, to)
compaction: SSTables merge, newest wins, tombstones dropped
  ok:   after compaction: deletes gone, overwrites win, originals intact
bloom filter: false-positive rate near the 1% target
  ok:   false-positive rate 1.030% is near the 1% target
```

## Performance

```
$ make bench     # 500k writes then random reads

  writes: 500,000 puts in 17.14 s = 29,166 puts/s (2 SSTables)
  reads:  200,000 random gets in 1.48 s = 135,414 gets/s
  misses: 200,000 absent-key gets in 0.07 s = 2,734,848 gets/s (0 false hits)
```

- **Writes** are durable — each one is flushed to the OS before returning — so ~29k/s is the honest cost of per-write durability (group commit would raise it, at the price of the guarantee above).
- **Reads** run at ~135k/s because each SSTable is **memory-mapped**: a lookup is a Bloom-filter probe, a sparse-index binary search, and a short scan through *memory*, not a syscall per field. (An earlier `RandomAccessFile` version managed only ~2.8k/s — the memory map is a 47× difference.)
- **Negative lookups** — keys that were never written — run at ~2.7M/s because the Bloom filters reject them without reading the SSTables at all.

## Using it

```java
try (LsmTree db = new LsmTree(new File("data/"))) {
    db.put("user:1", "alice");
    db.put("user:2", "bob");
    System.out.println(db.getString("user:1"));     // alice
    db.delete("user:2");                            // tombstone
    for (Entry e : db.scan("user:", "user:~"))      // ordered range scan
        System.out.println(e.key + " = " + new String(e.value));
}   // close() flushes the memtable to an SSTable
```

## Build & run

```bash
make test      # compile + run the full test suite (crash recovery, reference, compaction, bloom)
make bench      # compile + run the write/read benchmark
make clean
```

Needs a JDK 17+ (`java`/`javac`); no third-party dependencies. Tested with OpenJDK 21.

## Layout

| path | what it holds |
|---|---|
| `src/lsm/LsmTree.java` | the store: put/get/delete/scan, flush, compaction, open & crash recovery |
| `src/lsm/WriteAheadLog.java` | append-and-flush durability, and torn-record-tolerant replay |
| `src/lsm/MemTable.java` | the sorted in-memory write buffer (tombstone-aware) |
| `src/lsm/SSTable.java` | the immutable on-disk table: data, sparse index, bloom, footer; memory-mapped reads |
| `src/lsm/BloomFilter.java` | the from-scratch Bloom filter (double hashing) |
| `test/lsm/TestLsm.java` | crash recovery, the reference comparison, persistence, compaction, bloom stats |
| `bench/lsm/Bench.java` | write and read throughput |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the write and read paths, the SSTable file format, why deletes are tombstones, and how compaction keeps reads bounded.

## License

MIT — see [`LICENSE`](./LICENSE).
