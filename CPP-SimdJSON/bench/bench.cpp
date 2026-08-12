// Throughput benchmark.
//
// The SIMD here accelerates *scanning*: skipping whitespace, and finding the end
// of a string body. So it pays off in proportion to how much of the parse is
// spent scanning bytes rather than allocating nodes. We measure two documents to
// show both regimes honestly:
//
//   * "structural" — many small objects/arrays. This is allocation-bound (a
//     shared_ptr per container, a std::string per key), the scan runs are short,
//     and SIMD is roughly neutral.
//
//   * "text-heavy" — long string values and long whitespace runs. Here scanning
//     dominates and the AVX2 path is ~2x the scalar one.
//
// Both documents are parsed by the identical recursive-descent parser; the only
// difference between the two timings is whether the scanners are vectorised, so
// the ratio isolates exactly what the SIMD earns.

#include "simdjson.hpp"

#include <chrono>
#include <cstdio>
#include <string>

static std::string make_structural(int records) {
    std::string s = "[\n";
    for (int i = 0; i < records; ++i) {
        if (i) s += ",\n";
        s += "    {\n";
        s += "        \"id\": " + std::to_string(i) + ",\n";
        s += "        \"name\": \"record " + std::to_string(i) + "\",\n";
        s += "        \"active\": " + std::string(i % 2 ? "true" : "false") + ",\n";
        s += "        \"score\": " + std::to_string(i * 1.5) + ",\n";
        s += "        \"tags\": [\"a\", \"b\", \"c\"],\n";
        s += "        \"nested\": { \"x\": " + std::to_string(i) + ", \"y\": null }\n";
        s += "    }";
    }
    s += "\n]\n";
    return s;
}

static std::string make_text_heavy(int records) {
    // Long string values and generous leading whitespace — the work the SIMD
    // scanners exist to do.
    std::string s = "[\n";
    for (int i = 0; i < records; ++i) {
        if (i) s += ",";
        s += std::string(40, ' ');                       // a whitespace run to skip
        s += "\"" + std::string(600, 'x') + "\"";        // a long string body to scan
    }
    s += "\n]\n";
    return s;
}

static double time_parses(const std::string &doc, bool use_simd, int iters) {
    sj::Parser parser;
    auto t0 = std::chrono::steady_clock::now();
    volatile size_t sink = 0;
    for (int i = 0; i < iters; ++i) sink += parser.parse(doc, use_simd).size();
    auto t1 = std::chrono::steady_clock::now();
    (void)sink;
    return std::chrono::duration<double>(t1 - t0).count();
}

static void run(const char *label, const std::string &doc, int iters) {
    double mb = (double)doc.size() * iters / (1024.0 * 1024.0);
    time_parses(doc, true, 3);  // warm up
    double t_scalar = time_parses(doc, false, iters);
    double t_simd = time_parses(doc, true, iters);
    std::printf("%s  (%.2f MB x%d)\n", label, (double)doc.size() / (1024.0 * 1024.0), iters);
    std::printf("    scalar scan : %6.3f s   %7.1f MB/s\n", t_scalar, mb / t_scalar);
    std::printf("    AVX2 scan   : %6.3f s   %7.1f MB/s\n", t_simd, mb / t_simd);
    std::printf("    speedup     : %.2fx\n\n", t_scalar / t_simd);
}

int main() {
    run("structural JSON (allocation-bound)", make_structural(20000), 20);
    run("text-heavy JSON (scan-bound)",       make_text_heavy(2000),  40);
    return 0;
}
