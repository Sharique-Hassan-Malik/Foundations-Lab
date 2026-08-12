using System;
using System.Globalization;

namespace ZeroCsv;

/// <summary>
/// A zero-allocation, RFC 4180 CSV parser.
///
/// The whole parser is built from <c>ref struct</c>s over a
/// <see cref="ReadOnlySpan{Char}"/> of the source text, so it never copies a
/// field onto the heap: each field is handed back as a slice of the original
/// buffer. Parsing a megabyte of CSV and reading every field allocates zero
/// bytes — the property the tests measure with <c>GC.GetAllocatedBytes</c>.
///
/// It handles the full RFC 4180 grammar: quoted fields, doubled quotes as an
/// escape ("" → "), and commas or newlines embedded inside quotes. Only a field
/// that actually contains a doubled quote needs un-escaping into a caller buffer;
/// every other field is returned verbatim as a span.
///
///     foreach (var row in new CsvReader(text))
///         foreach (var field in row)
///             Use(field.Span);          // ReadOnlySpan&lt;char&gt;, no allocation
/// </summary>
public readonly ref struct CsvReader
{
    private readonly ReadOnlySpan<char> _text;
    private readonly char _delimiter;

    public CsvReader(ReadOnlySpan<char> text, char delimiter = ',')
    {
        _text = text;
        _delimiter = delimiter;
    }

    public RowEnumerator GetEnumerator() => new(_text, _delimiter);
}

/// <summary>Iterates the rows of a CSV document, respecting quoted newlines.</summary>
public ref struct RowEnumerator
{
    private readonly ReadOnlySpan<char> _text;
    private readonly char _delimiter;
    private int _pos;
    private CsvRow _current;

    internal RowEnumerator(ReadOnlySpan<char> text, char delimiter)
    {
        _text = text;
        _delimiter = delimiter;
        _pos = 0;
        _current = default;
    }

    public CsvRow Current => _current;

    public bool MoveNext()
    {
        if (_pos >= _text.Length) return false;

        int start = _pos;
        bool inQuotes = false;
        while (_pos < _text.Length)
        {
            char c = _text[_pos];
            if (inQuotes)
            {
                if (c == '"')
                {
                    // a doubled quote inside a quoted field is an escaped quote
                    if (_pos + 1 < _text.Length && _text[_pos + 1] == '"') { _pos += 2; continue; }
                    inQuotes = false;
                }
                _pos++;
            }
            else if (c == '"') { inQuotes = true; _pos++; }
            else if (c == '\n')
            {
                _current = new CsvRow(_text.Slice(start, _pos - start), _delimiter);
                _pos++;                                   // consume LF
                return true;
            }
            else if (c == '\r')
            {
                int end = _pos;
                _pos++;
                if (_pos < _text.Length && _text[_pos] == '\n') _pos++;   // consume CRLF
                _current = new CsvRow(_text.Slice(start, end - start), _delimiter);
                return true;
            }
            else _pos++;
        }

        _current = new CsvRow(_text.Slice(start, _pos - start), _delimiter);  // last row, no newline
        return true;
    }
}

/// <summary>One row: a slice of the document, split into fields on demand.</summary>
public readonly ref struct CsvRow
{
    private readonly ReadOnlySpan<char> _row;
    private readonly char _delimiter;

    internal CsvRow(ReadOnlySpan<char> row, char delimiter)
    {
        _row = row;
        _delimiter = delimiter;
    }

    public ReadOnlySpan<char> Raw => _row;
    public FieldEnumerator GetEnumerator() => new(_row, _delimiter);
}

/// <summary>Iterates the fields of a single row.</summary>
public ref struct FieldEnumerator
{
    private readonly ReadOnlySpan<char> _row;
    private readonly char _delimiter;
    private int _pos;
    private bool _emit;              // is a field still available to read?
    private CsvField _current;

    internal FieldEnumerator(ReadOnlySpan<char> row, char delimiter)
    {
        _row = row;
        _delimiter = delimiter;
        _pos = 0;
        _emit = true;               // an empty row still yields one empty field
        _current = default;
    }

    public CsvField Current => _current;

    public bool MoveNext()
    {
        if (!_emit) return false;

        if (_pos < _row.Length && _row[_pos] == '"')
        {
            // quoted field: content is between the quotes; note any "" escape
            _pos++;
            int contentStart = _pos;
            bool needsUnescape = false;
            while (_pos < _row.Length)
            {
                char c = _row[_pos];
                if (c == '"')
                {
                    if (_pos + 1 < _row.Length && _row[_pos + 1] == '"') { needsUnescape = true; _pos += 2; continue; }
                    break;                                // closing quote
                }
                _pos++;
            }
            _current = new CsvField(_row.Slice(contentStart, _pos - contentStart), needsUnescape);
            if (_pos < _row.Length && _row[_pos] == '"') _pos++;   // skip closing quote
        }
        else
        {
            int start = _pos;
            while (_pos < _row.Length && _row[_pos] != _delimiter) _pos++;
            _current = new CsvField(_row.Slice(start, _pos - start), false);
        }

        if (_pos < _row.Length && _row[_pos] == _delimiter) { _pos++; _emit = true; }
        else _emit = false;         // this was the last field in the row
        return true;
    }
}

/// <summary>A single field, as a slice of the source. Value types only — no heap.</summary>
public readonly ref struct CsvField
{
    private readonly ReadOnlySpan<char> _raw;

    internal CsvField(ReadOnlySpan<char> raw, bool needsUnescape)
    {
        _raw = raw;
        NeedsUnescape = needsUnescape;
    }

    /// <summary>True when the field contained a doubled quote and must be
    /// un-escaped (via <see cref="Unescape"/>) to get its real value.</summary>
    public bool NeedsUnescape { get; }

    /// <summary>The field content as a span. Equals the value unless
    /// <see cref="NeedsUnescape"/> is set.</summary>
    public ReadOnlySpan<char> Span => _raw;

    public int Length => _raw.Length;
    public bool IsEmpty => _raw.IsEmpty;

    /// <summary>Copy the un-escaped value ("" collapsed to ") into
    /// <paramref name="dest"/>; returns the number of characters written. No
    /// allocation. <paramref name="dest"/> must be at least <see cref="Length"/>.</summary>
    public int Unescape(Span<char> dest)
    {
        int w = 0;
        for (int i = 0; i < _raw.Length; i++)
        {
            char c = _raw[i];
            dest[w++] = c;
            if (c == '"' && i + 1 < _raw.Length && _raw[i + 1] == '"') i++;  // skip the pair's second quote
        }
        return w;
    }

    public bool TryParseInt(out int value) =>
        int.TryParse(_raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out value);

    public bool TryParseDouble(out double value) =>
        double.TryParse(_raw, NumberStyles.Float, CultureInfo.InvariantCulture, out value);

    /// <summary>Materialise the value as a string. This DOES allocate — use
    /// <see cref="Span"/> on the hot path.</summary>
    public override string ToString()
    {
        if (!NeedsUnescape) return new string(_raw);
        Span<char> buf = _raw.Length <= 256 ? stackalloc char[_raw.Length] : new char[_raw.Length];
        int n = Unescape(buf);
        return new string(buf[..n]);
    }
}
