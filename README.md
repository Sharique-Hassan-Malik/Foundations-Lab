# Polyglot Foundations

Performance-critical building blocks — the kind you find *inside* a database, a runtime, a compiler, or a collaborative editor — each implemented from scratch, and each proven with a **measured headline result** rather than a claim. Two projects per language: C, C++, C#, Java, and JavaScript.

A collection of 10 self-contained projects. Each lives in its own subdirectory with its own `README.md`, `ARCHITECTURE.md`, `LICENSE`, and tests, and builds and runs independently with only its language's standard toolchain — no third-party dependencies (the one exception being the C# source generator, which references Roslyn).

## Projects

| project | language | what it is | headline |
|---|---|---|---|
| [`C-Allocator`](./C-Allocator) | C | A `malloc`/`free`/`realloc` allocator: segregated free lists, boundary-tag coalescing, `mmap`-backed. | **0 corruptions** over 2,000,000 random ops with every heap invariant verified; ~2× glibc's speed, 76% utilisation. |
| [`C-Coroutines`](./C-Coroutines) | C | Stackful symmetric coroutines: a hand-written x86-64 context switch in assembly, guard-paged stacks. | **37.8M** context switches/sec (~26 ns); AddressSanitizer-clean. |
| [`CPP-WorkStealing`](./CPP-WorkStealing) | C++ | A work-stealing thread pool on a lock-free **Chase–Lev deque** (the Cilk / TBB / rayon / Go scheduler). | Every task run **exactly once** under contention (AddressSanitizer-clean); near-linear scaling to physical cores. |
| [`CPP-SimdJSON`](./CPP-SimdJSON) | C++ | A JSON parser whose whitespace/string scanners are accelerated with **AVX2 SIMD**; scalar fallback. | SIMD path **provably equal** to scalar on every input; ~2× throughput on scan-bound documents. |
| [`CSharp-ZeroCSV`](./CSharp-ZeroCSV) | C# | A zero-allocation RFC 4180 CSV parser built on `ReadOnlySpan<char>` and `ref struct`s. | Parses 4.5 MiB allocating **0 bytes** (vs 48.8 MiB for `string.Split`) at ~280 MiB/s. |
| [`CSharp-SourceGen`](./CSharp-SourceGen) | C# | A **Roslyn incremental source generator**: `[EnumExtensions]` → a compile-time `ToStringFast()`. | Generated `switch` is **3.9×** faster than reflection-based `Enum.ToString()`, verified identical. |
| [`Java-LSM`](./Java-LSM) | Java | A log-structured merge-tree key-value store: WAL, memtable, SSTables, bloom filters, compaction. | **Crash recovery** — every acknowledged write survives a crash via the WAL; ~135k reads/s (memory-mapped). |
| [`Java-BTree`](./Java-BTree) | Java | A generic **B+ tree**: split-on-insert, borrow/merge-on-delete, chained leaves for range scans. | Every structural invariant checked over millions of ops; 2M keys at height 4, ~1.3M puts/s. |
| [`JS-CRDT`](./JS-CRDT) | JavaScript | Conflict-free replicated data types: counters, an observed-remove set, and an RGA sequence for text. | Replicas **provably converge** under randomized delivery order (property-tested over 60 trials). |
| [`JS-Parser-Combinators`](./JS-Parser-Combinators) | JavaScript | A **parser-combinator** library, then a full JSON parser and an arithmetic evaluator built from it. | JSON parser matches `JSON.parse` across exhaustive escape/unicode cases; furthest-error reporting. |

## The common thread

Each project targets a real piece of systems infrastructure and pins its correctness to a number you can watch:

- **`C-Allocator`** — a signature written into every live allocation, re-checked on every touch: zero corruption across millions of operations.
- **`C-Coroutines`** — resume/yield ping-pong across a hand-rolled register switch, counted and timed under AddressSanitizer.
- **`CPP-WorkStealing`** — a per-task counter proving exactly-once execution while owner pops race against thieves' steals.
- **`CPP-SimdJSON`** — every input parsed twice, with SIMD and without, and the two DOMs asserted equal — the vectorised scan can't silently diverge.
- **`CSharp-ZeroCSV`** — `GC.GetAllocatedBytes` before and after the parse loop: exactly zero.
- **`CSharp-SourceGen`** — the generated `ToStringFast` checked against `Enum.ToString` for every member, then timed against it.
- **`Java-LSM`** — write, simulate a crash, reopen, and find every write recovered from the log.
- **`Java-BTree`** — full balance/occupancy/ordering/leaf-chain invariant check after millions of inserts and deletes.
- **`JS-CRDT`** — the same operations delivered to replicas in shuffled orders, then asserted byte-identical.
- **`JS-Parser-Combinators`** — the combinator-built JSON parser compared to `JSON.parse` across exhaustive escape and unicode cases.

They also share a build philosophy: **no dependencies** (bar Roslyn for the C# generator). Every project compiles and tests with just its language's toolchain, and every test suite is a plain runner (or the language's built-in one) that exits non-zero on failure.

## Building

Each subdirectory is standalone. Enter one and follow its README:

```bash
cd C-Allocator            && make test    # C     — gcc
cd C-Coroutines           && make test    # C     — gcc + as (x86-64)
cd CPP-WorkStealing       && make test    # C++   — g++ (C++17)
cd CPP-SimdJSON           && make test    # C++   — g++ (C++17, -mavx2)
cd CSharp-ZeroCSV         && dotnet run -c Release --project ZeroCsv.Tests    # C# — .NET 8
cd CSharp-SourceGen       && dotnet run -c Release --project EnumFast.Tests   # C# — .NET 8
cd Java-LSM               && make test    # Java  — JDK 17+
cd Java-BTree             && make test    # Java  — JDK 17+
cd JS-CRDT                && npm test     # JS    — Node 18+ (node:test)
cd JS-Parser-Combinators  && npm test     # JS    — Node 18+ (node:test)
```

## License

MIT — see the `LICENSE` file in each project.
