# Architecture

The whole parser is a small state machine expressed as nested `ref struct`s over a single `ReadOnlySpan<char>`. There is no buffering, no tokeniser object, and no per-field string — every field returned is a *view* into the caller's original text.

## Why `ref struct` is the whole trick

A `ReadOnlySpan<char>` is a `(pointer, length)` pair that may point at stack memory, so the runtime forbids it from ever landing on the heap — which means a type that *holds* a span must itself be stack-only: a `ref struct`. That constraint is exactly what makes zero-allocation parsing possible and safe:

- `CsvReader`, `RowEnumerator`, `CsvRow`, `FieldEnumerator`, and `CsvField` are all `ref struct`s. They live on the stack, are copied by value, and can carry a span without the GC ever being involved.
- C#'s `foreach` binds to `GetEnumerator`/`MoveNext`/`Current` structurally, not through `IEnumerable`. Because these enumerators are structs, `foreach` neither boxes them nor allocates an iterator — the two nested `foreach` loops compile to plain index advancing over the span.

The cost of the constraint is that a `ref struct` can't be stored in a field of a normal class, put in a `List<T>`, captured by a lambda, or used across an `await`. For a streaming parser that's no loss: you consume each field inside the loop.

## Two levels: rows, then fields

Splitting CSV is not just "split on `\n` then split on `,`", because a newline or comma inside quotes is data, not a separator. So the parse is two passes, each a simple linear scan that tracks quote state:

**Row scan (`RowEnumerator`).** Walk from the current position keeping an `inQuotes` flag. A `"` toggles it (a doubled `""` while inside is skipped as an escape, not a toggle). Only an *unquoted* `\n` or `\r\n` ends a row; the row is the slice from the start up to that newline, and the newline is consumed. At end of input the final slice is returned even without a trailing newline — and a trailing newline yields no phantom empty row.

**Field scan (`FieldEnumerator`).** Given one row's slice, walk it splitting on *unquoted* delimiters. A field that starts with `"` is quoted: its content is the span between the quotes, and if a `""` was seen along the way the field is flagged `NeedsUnescape`. Otherwise the field runs to the next delimiter. An `_emit` flag drives the tricky boundary cases correctly: consuming a delimiter means another field follows (so `a,` yields `["a", ""]` — a real trailing empty field), while reaching the row's end without a delimiter means the field just produced was the last one. An empty row yields a single empty field.

The result of both scans is always a sub-span of the original text, so no bytes are copied.

## The one place a copy can happen

RFC 4180 escapes a literal quote by doubling it: the field `"she said ""hi"""` means `she said "hi"`. The *raw* span between the outer quotes is `she said ""hi""` — to hand back the true value you must collapse each `""` to `"`, and the collapsed string is shorter, so it cannot be a slice of the original. ZeroCsv refuses to allocate for this implicitly. Instead:

- `CsvField.NeedsUnescape` tells you whether the raw span is already the value (the common case — most fields have no doubled quotes) or needs collapsing;
- `CsvField.Unescape(Span<char> dest)` writes the collapsed value into a **caller-provided** buffer and returns its length — the caller decides where that memory lives (a stack `stackalloc`, a pooled array, …), so the parser itself still allocates nothing;
- only the convenience `ToString()` allocates a `string`, and it's clearly documented as the off-the-hot-path escape hatch.

So the zero-allocation guarantee is precise: reading fields as spans and parsing numbers off them (via `int.TryParse(ReadOnlySpan<char>, …)`, which is itself allocation-free) touches the heap zero times, which is what `GC.GetAllocatedBytesForCurrentThread()` confirms across the parse loop. Any allocation is something the *caller* opted into.
