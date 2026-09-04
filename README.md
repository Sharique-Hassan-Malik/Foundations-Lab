# Polyglot Foundations

Performance-critical building blocks — the kind you find *inside* a database, a runtime, a compiler, or a collaborative editor — each implemented from scratch, and each proven with a **measured headline result** rather than a claim. Thirteen projects across five languages: C, C++, C#, Java, and JavaScript.

A collection of 13 self-contained projects. Each lives in its own subdirectory with its own `README.md`, `ARCHITECTURE.md`, `LICENSE`, and tests, and builds and runs independently with only its language's standard toolchain. Two carry a third-party dependency, both deliberate: the C# source generator references Roslyn, and `Promises-APlus` pulls the official Promises/A+ conformance suite as a dev dependency — the point of that project is to be judged by the specification's own test suite rather than by one of its own.

## Projects

| project | language | what it is | headline |
|---|---|---|---|
| [`C-Allocator`](./C-Allocator) | C | A `malloc`/`free`/`realloc` allocator: segregated free lists, boundary-tag coalescing, `mmap`-backed. | **0 corruptions** over 2,000,000 random ops with every heap invariant verified; ~2× glibc's speed, 76% utilisation. |
| [`C-Coroutines`](./C-Coroutines) | C | Stackful symmetric coroutines: a hand-written x86-64 context switch in assembly, guard-paged stacks. | **37.8M** context switches/sec (~26 ns); AddressSanitizer-clean. |
| [`C-Deflate`](./C-Deflate) | C | DEFLATE (RFC 1951) from scratch: LZ77 hash-chain matching, canonical Huffman, all three block types. | **28/28** streams accepted by `gunzip`; within **0.2%** of `gzip -9` on its own source. |
| [`CPP-WorkStealing`](./CPP-WorkStealing) | C++ | A work-stealing thread pool on a lock-free **Chase–Lev deque** (the Cilk / TBB / rayon / Go scheduler). | Every task run **exactly once** under contention (AddressSanitizer-clean); near-linear scaling to physical cores. |
| [`CPP-SimdJSON`](./CPP-SimdJSON) | C++ | A JSON parser whose whitespace/string scanners are accelerated with **AVX2 SIMD**; scalar fallback. | SIMD path **provably equal** to scalar on every input; ~2× throughput on scan-bound documents. |
| [`CSharp-ZeroCSV`](./CSharp-ZeroCSV) | C# | A zero-allocation RFC 4180 CSV parser built on `ReadOnlySpan<char>` and `ref struct`s. | Parses 4.5 MiB allocating **0 bytes** (vs 48.8 MiB for `string.Split`) at ~280 MiB/s. |
| [`CSharp-SourceGen`](./CSharp-SourceGen) | C# | A **Roslyn incremental source generator**: `[EnumExtensions]` → a compile-time `ToStringFast()`. | Generated `switch` is **3.9×** faster than reflection-based `Enum.ToString()`, verified identical. |
| [`Java-LSM`](./Java-LSM) | Java | A log-structured merge-tree key-value store: WAL, memtable, SSTables, bloom filters, compaction. | **Crash recovery** — every acknowledged write survives a crash via the WAL; ~135k reads/s (memory-mapped). |
| [`Java-BTree`](./Java-BTree) | Java | A generic **B+ tree**: split-on-insert, borrow/merge-on-delete, chained leaves for range scans. | Every structural invariant checked over millions of ops; 2M keys at height 4, ~1.3M puts/s. |
| [`JS-CRDT`](./JS-CRDT) | JavaScript | Conflict-free replicated data types: counters, an observed-remove set, and an RGA sequence for text. | Replicas **provably converge** under randomized delivery order (property-tested over 60 trials). |
| [`Promises-APlus`](./Promises-APlus) | JavaScript | A `Promise` built to the Promises/A+ specification: three states, microtask-scheduled handlers, thenable adoption. | **872/872** official A+ conformance tests pass — the specification's own executable definition of correct. |
| [`Reactive-Signals`](./Reactive-Signals) | JavaScript | Fine-grained reactivity — `signal` / `computed` / `effect` — with lazy pull-based recomputation. | **Glitch-free and linear**: 61 recomputations where eager propagation needs 4.2M, on a depth-20 diamond. |
| [`HyperLogLog`](./HyperLogLog) | Java | Cardinality estimation from a bounded sketch: harmonic-mean estimator, bias correction, mergeable registers. | 10M distinct items counted to **0.40%** error in a fixed **16 KB**; RMS error tracks the `1.04/√m` bound. |

## The common thread

Each project targets a real piece of systems infrastructure and pins its correctness to a number you can watch:

- **`C-Allocator`** — a signature written into every live allocation, re-checked on every touch: zero corruption across millions of operations.
- **`C-Coroutines`** — resume/yield ping-pong across a hand-rolled register switch, counted and timed under AddressSanitizer.
- **`C-Deflate`** — the output handed to `gunzip`, a program that has never seen the code: a codec whose encoder and decoder share a bug round-trips perfectly and fails here.
- **`CPP-WorkStealing`** — a per-task counter proving exactly-once execution while owner pops race against thieves' steals.
- **`CPP-SimdJSON`** — every input parsed twice, with SIMD and without, and the two DOMs asserted equal — the vectorised scan can't silently diverge.
- **`CSharp-ZeroCSV`** — `GC.GetAllocatedBytes` before and after the parse loop: exactly zero.
- **`CSharp-SourceGen`** — the generated `ToStringFast` checked against `Enum.ToString` for every member, then timed against it.
- **`Java-LSM`** — write, simulate a crash, reopen, and find every write recovered from the log.
- **`Java-BTree`** — full balance/occupancy/ordering/leaf-chain invariant check after millions of inserts and deletes.
- **`JS-CRDT`** — the same operations delivered to replicas in shuffled orders, then asserted byte-identical.
- **`Promises-APlus`** — the official 872-test A+ conformance suite, run against the implementation on every `npm run conformance`.
- **`Reactive-Signals`** — the diamond benchmark, counting recomputations: linear in depth where the eager approach is exponential.
- **`HyperLogLog`** — estimates against known true counts over 40 independent trials, with the RMS error held to the theoretical bound.

They also share a build philosophy: **no runtime dependencies anywhere**, and no build-time ones beyond the two noted above. Every project compiles and tests with just its language's toolchain, and every test suite is a plain runner (or the language's built-in one) that exits non-zero on failure.

## Building

Each subdirectory is standalone. Enter one and follow its README:

```bash
cd C-Allocator            && make test    # C     — gcc
cd C-Coroutines           && make test    # C     — gcc + as (x86-64)
cd C-Deflate              && make test    # C     — gcc
cd C-Deflate              && make gzcheck  # C     — gcc + gzip
cd CPP-WorkStealing       && make test    # C++   — g++ (C++17)
cd CPP-SimdJSON           && make test    # C++   — g++ (C++17, -mavx2)
cd CSharp-ZeroCSV         && dotnet run -c Release --project ZeroCsv.Tests    # C# — .NET 8
cd CSharp-SourceGen       && dotnet run -c Release --project EnumFast.Tests   # C# — .NET 8
cd Java-LSM               && make test    # Java  — JDK 17+
cd Java-BTree             && make test    # Java  — JDK 17+
cd HyperLogLog            && ./build.sh   # Java  — JDK 17+
cd JS-CRDT                && npm test     # JS    — Node 18+ (node:test)
cd Reactive-Signals       && npm test     # JS    — Node 18+ (node:test)
cd Promises-APlus         && npm test     # JS    — Node 18+ (node:test)
```

`Promises-APlus` additionally runs the official conformance suite, which is the
one place a network fetch is needed:

```bash
cd Promises-APlus && npm install && npm run conformance   # 872 A+ tests
```

## License

MIT — see the [`LICENSE`](./LICENSE) file at the root, and the copy in each project.
