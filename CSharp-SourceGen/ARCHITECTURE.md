# Architecture

A source generator is a compiler plug-in: it runs *inside* the C# compilation,
looks at the code being compiled, and adds more code to that same compilation
before it is turned into IL. So the code it emits is indistinguishable from
hand-written code — it is type-checked, optimised, and stepped-through like any
other — but it is written by a program at build time instead of by a person.

## Why netstandard2.0, and the `IsExternalInit` polyfill

A generator is loaded and executed by the compiler (Roslyn) itself, which runs on
the .NET runtime that hosts the build. To be loadable by every SDK, an
analyzer/generator assembly must target **netstandard2.0** — the lowest common
denominator. That framework predates C# 9, so `record` types and `init`-only
setters, which the compiler lowers to a call on `System.Runtime.CompilerServices.IsExternalInit`,
won't compile against it unless that type exists. Declaring a one-line internal
`IsExternalInit` (the standard polyfill) brings records back, which is worth it
because the generator's data model wants to be a value type (see below).

## The incremental pipeline

The generator implements **`IIncrementalGenerator`**, the modern API built around
caching. Instead of re-scanning the whole syntax tree on every keystroke, it
declares a *pipeline* of transformations, and Roslyn memoises each stage: if a
stage's input is equal (by value) to what it saw last time, the cached output is
reused and downstream work is skipped. For an editor doing this on every edit,
that is the difference between snappy and unusable.

Three pieces make up the pipeline here:

1. **`RegisterPostInitializationOutput`** emits the `[EnumExtensions]` marker
   attribute into the compilation before any analysis. Emitting the attribute
   from the generator itself means a consumer needs no separate "attributes"
   reference assembly — the marker simply exists once the generator is referenced.

2. **`ForAttributeWithMetadataName`** is the efficient entry point (Roslyn 4.x):
   rather than visiting every node and asking "is this an annotated enum?", it
   pre-indexes attribute usages and invokes the callback *only* for nodes that
   carry the named attribute. That indexing is what keeps a generator cheap in a
   large codebase.

3. **`RegisterSourceOutput`** takes the projected models and produces one source
   file per enum.

## Why the model must be a value

The caching in step 2/3 only helps if Roslyn can cheaply tell whether a stage's
output changed — and it does that with equality. So the transform deliberately
projects each enum down to a small **immutable, value-equatable record**
(`EnumModel`: namespace, name, full name, accessibility, and the deduplicated
member names) and holds on to *nothing else*. It does **not** keep the
`INamedTypeSymbol` or any syntax node, because those are tied to a specific
compilation and never compare equal across edits — capturing them would defeat
caching and, worse, leak large object graphs. Reducing to a value type at the
boundary is the single most important discipline in writing a well-behaved
incremental generator.

## Generating the code

`Extract` reads the enum symbol: its namespace (or global), its name, whether it
is `public` (so the generated class can match its accessibility), and its
members. Enum members are `IFieldSymbol`s with a constant value; the extractor
deduplicates by that constant value, because C# allows several names to share one
underlying value (aliases) and two `switch` arms with the same label is a compile
error — keeping the first name is both correct and matches what `Enum.ToString`
returns for that value.

`Generate` then renders a `switch` expression mapping each member to
`nameof(Enum.Member)` (a compile-time constant string), with a `_ => value.ToString()`
arm so undefined values behave exactly like the framework. The result is a static
class of extension methods, fully qualified with `global::` so it can't be
tripped up by ambient `using`s or same-named types in the consumer.
