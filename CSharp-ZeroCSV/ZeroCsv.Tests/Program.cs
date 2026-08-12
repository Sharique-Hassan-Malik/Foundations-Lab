using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using ZeroCsv;

// Tests + zero-allocation proof + throughput, as a plain console runner (exit
// code 0 on success) so the project needs no test-framework NuGet restore.

int failures = 0;
void Check(bool cond, string msg)
{
    Console.WriteLine((cond ? "  ok:   " : "  FAIL: ") + msg);
    if (!cond) failures++;
}

// collect every field as a string (for assertions; this allocates — the point
// of the parser is that *it* doesn't, which the zero-alloc section proves)
static List<List<string>> Parse(string text)
{
    var rows = new List<List<string>>();
    foreach (var row in new CsvReader(text))
    {
        var fields = new List<string>();
        foreach (var f in row) fields.Add(f.ToString());
        rows.Add(fields);
    }
    return rows;
}

static bool RowEq(List<string> got, params string[] want)
{
    if (got.Count != want.Length) return false;
    for (int i = 0; i < want.Length; i++) if (got[i] != want[i]) return false;
    return true;
}

Console.WriteLine("RFC 4180 correctness");

var r = Parse("a,b,c\n1,2,3\n");
Check(r.Count == 2, "two rows, trailing newline ignored");
Check(RowEq(r[0], "a", "b", "c"), "simple header row");
Check(RowEq(r[1], "1", "2", "3"), "simple data row");

r = Parse("x,y,z");           // no trailing newline
Check(r.Count == 1 && RowEq(r[0], "x", "y", "z"), "last row without newline");

r = Parse("a,,c\n");
Check(RowEq(r[0], "a", "", "c"), "empty field between delimiters");

r = Parse("a,b,\n");
Check(RowEq(r[0], "a", "b", ""), "trailing empty field");

r = Parse("\"hello, world\",2\n");
Check(RowEq(r[0], "hello, world", "2"), "comma inside quotes");

r = Parse("\"line one\nline two\",next\n");
Check(r.Count == 1, "newline inside quotes does not split the row");
Check(RowEq(r[0], "line one\nline two", "next"), "embedded newline preserved");

r = Parse("\"she said \"\"hi\"\"\",ok\n");
Check(RowEq(r[0], "she said \"hi\"", "ok"), "doubled quotes un-escaped");

r = Parse("a,b\r\nc,d\r\n");    // CRLF
Check(r.Count == 2 && RowEq(r[0], "a", "b") && RowEq(r[1], "c", "d"), "CRLF line endings");

r = Parse("\"\",\"x\"\n");
Check(RowEq(r[0], "", "x"), "empty quoted field");

// numeric parsing straight off the span
int parsed = 0; bool numsOk = true;
foreach (var row in new CsvReader("10,20,30\n40,50,60"))
    foreach (var f in row) { if (f.TryParseInt(out int v)) { parsed++; if (v % 10 != 0) numsOk = false; } }
Check(parsed == 6 && numsOk, "TryParseInt reads integers from spans");

// -------------------------------------------------------- zero allocation

Console.WriteLine("\nzero-allocation parse (the headline)");

var sb = new StringBuilder();
int nrows = 200_000;
for (int i = 0; i < nrows; i++)
    sb.Append(i).Append(",item").Append(i).Append(',').Append(i * 3).Append('\n');
string big = sb.ToString();

long SumThirdColumn()
{
    long s = 0;
    foreach (var row in new CsvReader(big))
    {
        int col = 0;
        foreach (var f in row)
        {
            if (col == 2 && f.TryParseInt(out int v)) s += v;
            col++;
        }
    }
    return s;
}

long warm = SumThirdColumn();                       // JIT + verify correctness
long expected = 0; for (long i = 0; i < nrows; i++) expected += i * 3;
Check(warm == expected, "parsed sum of the third column is correct");

long before = GC.GetAllocatedBytesForCurrentThread();
long got = SumThirdColumn();
long after = GC.GetAllocatedBytesForCurrentThread();
long allocated = after - before;
Check(got == expected, "second pass sum still correct");
Check(allocated < 256, $"parsing {big.Length / 1_048_576.0:F1} MiB allocated {allocated} bytes (≈ 0)");

// contrast: the naive String.Split approach on the same data
long beforeSplit = GC.GetAllocatedBytesForCurrentThread();
long ssum = 0;
foreach (var line in big.Split('\n'))
{
    if (line.Length == 0) continue;
    var parts = line.Split(',');
    ssum += long.Parse(parts[2]);
}
long afterSplit = GC.GetAllocatedBytesForCurrentThread();
Console.WriteLine($"  (for contrast, string.Split allocated {(afterSplit - beforeSplit) / 1_048_576.0:F1} MiB " +
                  $"for the same {nrows} rows)");

// -------------------------------------------------------- throughput

Console.WriteLine("\nthroughput");
var sw = Stopwatch.StartNew();
int reps = 20;
long acc = 0;
for (int i = 0; i < reps; i++) acc += SumThirdColumn();
sw.Stop();
double mib = big.Length * 2.0 / 1_048_576.0 * reps;   // chars are 2 bytes
Console.WriteLine($"  parsed {mib:F0} MiB of char data in {sw.Elapsed.TotalSeconds:F3} s " +
                  $"= {mib / sw.Elapsed.TotalSeconds:F0} MiB/s");
GC.KeepAlive(acc);

Console.WriteLine(failures == 0 ? "\nALL TESTS PASSED" : $"\n{failures} TEST(S) FAILED");
return failures == 0 ? 0 : 1;
