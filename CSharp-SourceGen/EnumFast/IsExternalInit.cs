// Polyfill: `record` and `init`-only setters need this type, which netstandard2.0
// (the target framework required for Roslyn analyzers/generators) does not ship.
namespace System.Runtime.CompilerServices
{
    internal static class IsExternalInit { }
}
