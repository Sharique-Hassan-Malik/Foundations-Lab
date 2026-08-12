# C# Zero-Allocation CSV Parser

A **zero-allocation CSV parser** in C#, built entirely from `ref struct`s over `ReadOnlySpan<char>`. It parses the full RFC 4180 grammar — quoted fields, doubled-quote escaping (`""` → `"`), and commas or newlines embedded inside quotes — and hands every field back as a *slice of the original buffer*, so reading a whole document allocates **nothing on the heap**.

## The headline: 0 bytes allocated

The claim is measured, not asserted. The test parses a 4.5 MiB CSV and sums a numeric column, with `GC.GetAllocatedBytesForCurrentThread()` sampled either side of the parse loop:

```
$ dotnet run -c Release --project ZeroCsv.Tests

zero-allocation parse (the headline)
  ok:   parsed sum of the third column is correct
  ok:   second pass sum still correct
  ok:   parsing 4.5 MiB allocated 0 bytes (≈ 0)
  (for contrast, string.Split allocated 48.8 MiB for the same 200000 rows)

throughput
  parsed 181 MiB of char data in 0.644 s = 281 MiB/s
```

**Zero bytes** for the whole parse. The idiomatic `string.Split` approach allocates **48.8 MiB** on the same input — a string per line and a string per field, all of which the GC then has to collect. ZeroCsv touches the garbage collector not at all, and still runs at ~280 MiB/s.

## Correctness

Every RFC 4180 case the parser must get right is a test, all passing:

```
RFC 4180 correctness
  ok:   two rows, trailing newline ignored          ok:   comma inside quotes
  ok:   last row without newline                     ok:   newline inside quotes does not split the row
  ok:   empty field between delimiters               ok:   embedded newline preserved
  ok:   trailing empty field                         ok:   doubled quotes un-escaped
  ok:   CRLF line endings                            ok:   empty quoted field
```

## Using it

```csharp
using ZeroCsv;

ReadOnlySpan<char> text = File.ReadAllText("data.csv");   // or any span

foreach (var row in new CsvReader(text))
{
    foreach (var field in row)
    {
        ReadOnlySpan<char> value = field.Span;            // a slice — no allocation
        if (field.TryParseInt(out int n)) { /* … */ }     // parse numbers off the span
        // field.NeedsUnescape is true only when the value contained "";
        // field.Unescape(dest) writes the real value into a caller buffer.
    }
}
```

Because the enumerators are `ref struct`s (stack-only, can hold a `ReadOnlySpan`), the familiar `foreach` works with no boxing and no iterator allocation. Un-escaping a `""`-containing field is the only case that can copy, and even then only into a caller-supplied `Span<char>` — never the heap.

## Build & run

```bash
dotnet run -c Release --project ZeroCsv.Tests   # builds the library + runs tests, zero-alloc proof, and benchmark
dotnet build -c Release ZeroCsv/ZeroCsv.csproj  # just the library
```

Requires the .NET 8 SDK. No third-party packages — the test runner is a plain console app, so there is nothing to restore.

## Layout

| path | what it holds |
|---|---|
| `ZeroCsv/CsvReader.cs` | the parser: `CsvReader` → `RowEnumerator` → `CsvRow` → `FieldEnumerator` → `CsvField`, all `ref struct` |
| `ZeroCsv.Tests/Program.cs` | RFC 4180 correctness, the `GC.GetAllocatedBytes` zero-alloc proof, and the throughput benchmark |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for how the two-level row/field enumeration works, why `ref struct` is the enabling feature, and where the one optional allocation lives.

## License

MIT — see [`LICENSE`](./LICENSE).
