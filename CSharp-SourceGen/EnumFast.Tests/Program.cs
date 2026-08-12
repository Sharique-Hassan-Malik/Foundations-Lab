using System;
using System.Diagnostics;
using EnumFast;

// Enums annotated with the generated marker attribute. The generator turns each
// of these into a "<Name>Extensions" static class with ToStringFast()/IsDefined().
[EnumExtensions] public enum Color { Red, Green, Blue, Yellow, Cyan, Magenta }
[EnumExtensions] internal enum Direction { North, East, South, West }
[EnumExtensions] public enum Status { Ok = 0, Warn = 1, Error = 2, Fatal = 2 }  // Fatal aliases Error

internal static class Program
{
    private static int _failures;

    private static void Check(bool cond, string msg)
    {
        Console.WriteLine((cond ? "  ok:   " : "  FAIL: ") + msg);
        if (!cond) _failures++;
    }

    private static int Main()
    {
        Console.WriteLine("generated ToStringFast / IsDefined");

        Check(Color.Green.ToStringFast() == "Green", "ToStringFast returns the member name");

        bool all = true;
        foreach (Color c in Enum.GetValues<Color>())
            if (c.ToStringFast() != c.ToString()) all = false;
        Check(all, "ToStringFast matches Enum.ToString for every member");

        Check(((Color)99).ToStringFast() == "99", "an undefined value falls back to ToString()");
        Check(Color.Blue.IsDefined(), "IsDefined is true for a declared value");
        Check(!((Color)99).IsDefined(), "IsDefined is false for an undefined value");
        Check(Direction.West.ToStringFast() == "West", "works for an internal enum too");
        Check(Status.Error.ToStringFast() == Status.Error.ToString(), "aliased members compile and agree with ToString");

        // ---------------------------------------------------- benchmark
        const int N = 20_000_000;
        long sink = 0;
        for (int i = 0; i < 100_000; i++)               // warm up the JIT
        { sink += ((Color)(i % 6)).ToStringFast().Length; sink += ((Color)(i % 6)).ToString().Length; }

        var sw = Stopwatch.StartNew();
        for (int i = 0; i < N; i++) sink += ((Color)(i % 6)).ToString().Length;
        double slow = sw.Elapsed.TotalSeconds;

        sw.Restart();
        for (int i = 0; i < N; i++) sink += ((Color)(i % 6)).ToStringFast().Length;
        double fast = sw.Elapsed.TotalSeconds;

        Console.WriteLine($"\nbenchmark — {N:N0} conversions:");
        Console.WriteLine($"  Enum.ToString() : {slow:F3} s");
        Console.WriteLine($"  ToStringFast()  : {fast:F3} s");
        Console.WriteLine($"  speedup         : {slow / fast:F1}x");
        GC.KeepAlive(sink);

        Console.WriteLine(_failures == 0 ? "\nALL TESTS PASSED" : $"\n{_failures} TEST(S) FAILED");
        return _failures == 0 ? 0 : 1;
    }
}
