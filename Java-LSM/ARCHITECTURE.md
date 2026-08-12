# Architecture

An LSM tree turns random writes into sequential ones and pays for it with a
slightly more expensive read. Everything below follows from that trade.

```
        put/delete                                   get
            │                                          │
            ▼                                          ▼
   ┌─────────────────┐  append+flush           check memtable (newest writes)
   │ write-ahead log │◀───────────────┐              │  miss
   └─────────────────┘                │              ▼
            │ then                     │        each SSTable, newest → oldest:
            ▼                          │          bloom? ── no ──▶ skip
   ┌─────────────────┐   when full     │          │ maybe
   │    memtable      │ ── flush ──▶ SSTable ──────┘  sparse index → seek → scan
   │ (sorted, in RAM) │                (immutable, on disk, memory-mapped)
   └─────────────────┘   truncate log
                        compaction merges SSTables
```

## The write path

`put`/`delete` do two things, in this order:

1. **Append to the write-ahead log and flush it to the OS.** The record — op byte, key, and (for a put) value — is durable before the call returns. This is the entire basis of the crash-recovery guarantee.
2. **Update the memtable**, a sorted `TreeMap`. A delete stores a `null` value — a *tombstone* — which is deliberately different from the key being absent: the tombstone has to exist so that a delete can shadow an older value that still lives in an SSTable on disk.

When the memtable's estimated size crosses a threshold it is **flushed**: written out in one sorted pass as a new SSTable, after which the log is truncated (those writes are now durable in the SSTable) and a fresh memtable begins. Writes therefore never seek — they are a log append plus an in-memory insert — which is why LSM trees ingest so fast.

## The SSTable, and the read path

An SSTable is immutable and sorted, with four sections and a fixed footer:

```
[ data:  keyLen|key|flag|(valLen|val) … in ascending key order ]
[ index: every 64th key with its byte offset                   ]
[ bloom: the serialized Bloom filter over all keys             ]
[ footer: indexOffset | bloomOffset | count | magic  (28 B)    ]
```

On open, the tiny index and bloom sections are read into memory and the whole file is **memory-mapped**. A `get` then costs:

1. **Bloom filter** — a handful of bit tests. If it says "no," the key is *certainly* not in this table and the table is skipped with zero reads. This is what makes negative lookups nearly free and keeps reads cheap even with many SSTables.
2. **Sparse index** — a `floorEntry` (binary search) finds the offset of the nearest earlier sampled key. The index is sparse (one entry per 64 keys) so it stays small; the price is a short linear scan.
3. **Scan** — from that offset, walk forward through the memory-mapped bytes comparing keys, stopping at a match or as soon as a larger key proves the target absent. Because the data is mapped, each field read is a memory access, not a syscall — the difference between ~2.8k and ~135k lookups a second measured in the benchmark.

The tree checks sources newest-first — memtable, then SSTables from newest to oldest generation — and returns the first hit, so a newer put or a tombstone always wins over an older value.

## Deletes, and why compaction is necessary

A delete is a tombstone, not an erasure, because the value it hides may sit in an older SSTable that can't be edited in place. Overwrites work the same way: a new put in a newer table shadows the old one. Left alone, this means dead data — superseded values and tombstones — accumulates across SSTables, slowing reads and wasting space.

**Compaction** reclaims it. When the SSTable count exceeds a threshold, all of them are merged: entries are folded oldest-to-newest into a sorted map so the newest version of each key wins, tombstones are dropped (safe in a full merge, since no older value survives it to need shadowing), and the result is written as a single new SSTable; the old files are then deleted. The compaction test checks exactly this end state — deleted keys stay gone, overwrites win, untouched originals remain — and that the SSTable count stays bounded.

## Recovery

Opening a tree does two things. It loads the existing SSTables, ordering them by the generation number in their filenames so the newest is consulted first. Then it **replays the write-ahead log** into a fresh memtable, re-applying every put and delete that had been acknowledged but not yet flushed to an SSTable. Replay is tolerant of a **torn final record** — a write interrupted mid-append by the crash — by stopping cleanly at the first short read, so a partially-written last entry never corrupts recovery. The result is that the reopened tree contains exactly the writes that were acknowledged before the crash, which is the durability property the headline test exercises.
