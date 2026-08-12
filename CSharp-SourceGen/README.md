# C# Source Generator (EnumFast)

A **Roslyn incremental source generator** in C#. Mark an enum `[EnumExtensions]` and, at *compile time*, the generator writes a `ToStringFast()` extension for it — a plain `switch` that returns the member name as a string literal — plus an `IsDefined()`. No reflection, no allocation, no runtime cost beyond a jump table.

## The headline: generated code that's ~4× faster than the framework

The built-in `Enum.ToString()` is reflection-based: every call looks the value up in the enum's metadata. The generated `switch` is resolved at compile time, so it compiles to a comparison and a literal. The test app measures both on 20 million conversions:

```
$ dotnet run -c Release --project EnumFast.Tests

generated ToStringFast / IsDefined
  ok:   ToStringFast returns the member name
  ok:   ToStringFast matches Enum.ToString for every member
  ok:   an undefined value falls back to ToString()
  ok:   IsDefined is true for a declared value / false for an undefined value
  ok:   works for an internal enum too
  ok:   aliased members compile and agree with ToString

benchmark — 20,000,000 conversions:
  Enum.ToString() : 0.871 s
  ToStringFast()  : 0.225 s
  speedup         : 3.9x
```

`ToStringFast` is verified to return exactly what `Enum.ToString` does for every member (and to fall back to `ToString()` for undefined values), so it's a drop-in — just faster.

## What you write, and what the generator writes

You write:

```csharp
[EnumExtensions]
public enum Color { Red, Green, Blue, Yellow, Cyan, Magenta }
```

The generator produces (into the compilation, no file on disk needed):

```csharp
public static class ColorExtensions
{
    public static string ToStringFast(this global::Color value)
        => value switch
        {
            global::Color.Red     => nameof(global::Color.Red),
            global::Color.Green   => nameof(global::Color.Green),
            // …
            _ => value.ToString(),
        };

    public static bool IsDefined(this global::Color value)
        => value switch { global::Color.Red => true, /* … */ _ => false };
}
```

It handles namespaces, `internal` enums (the generated class matches the enum's accessibility), and **aliased members** — two names sharing one underlying value would make two `switch` arms with the same label (a compile error), so the generator keeps the first and drops the duplicate.

## How it works

`EnumExtensionsGenerator` is an **`IIncrementalGenerator`**, so it plugs into Roslyn's caching pipeline and only re-runs for enums that actually changed. It:

1. emits the `[EnumExtensions]` marker attribute itself, via `RegisterPostInitializationOutput`, so consumers need no extra reference;
2. uses `ForAttributeWithMetadataName` to find annotated enums efficiently (Roslyn only invokes it for nodes carrying the attribute);
3. projects each enum to a tiny immutable model (name, namespace, accessibility, deduplicated member names) — deliberately *not* holding on to syntax or symbols, so the pipeline can cache well;
4. renders the `switch`-based source for each.

## Build & run

```bash
dotnet run -c Release --project EnumFast.Tests   # builds the generator, runs it, tests + benchmarks
dotnet build -c Release EnumFast/EnumFast.csproj  # just the generator (a netstandard2.0 analyzer)
```

Requires the .NET 8 SDK; the generator references `Microsoft.CodeAnalysis.CSharp` (restored from NuGet). The consumer references the generator as an analyzer — no runtime dependency ships with your app.

## Layout

| path | what it holds |
|---|---|
| `EnumFast/EnumExtensionsGenerator.cs` | the incremental generator: find annotated enums → emit `ToStringFast`/`IsDefined` |
| `EnumFast/IsExternalInit.cs` | a one-type polyfill so `record`/`init` work on netstandard2.0 |
| `EnumFast.Tests/Program.cs` | enums using the attribute, correctness checks, and the benchmark |

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for why generators target netstandard2.0, how the incremental pipeline caches, and why the model must be a value type.

## License

MIT — see [`LICENSE`](./LICENSE).
