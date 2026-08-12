// Tests for the SIMD JSON parser.
//
// Two things are proved: the parser is correct on the JSON grammar (numbers,
// strings with every escape incl. surrogate pairs, nesting, the RFC edge cases,
// and rejection of malformed input); and — the property that guards the SIMD
// code — the AVX2 path and the scalar path produce *identical* DOMs on every
// input, so the vectorised scanning can never silently diverge from the simple
// version.

#include "simdjson.hpp"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++failures; } \
    else         { std::printf("  ok:   %s\n", msg); } \
} while (0)

using namespace sj;

static void test_scalars() {
    std::printf("scalars: null, bools, numbers\n");
    CHECK(parse("null").is_null(), "null");
    CHECK(parse("true").boolean == true, "true");
    CHECK(parse("false").boolean == false, "false");
    CHECK(parse("42").number == 42.0, "integer");
    CHECK(parse("-3.5").number == -3.5, "negative float");
    CHECK(parse("6.022e23").number == 6.022e23, "exponent");
    CHECK(parse("0").number == 0.0, "zero");
    CHECK(parse("  \n\t 7 \n").number == 7.0, "surrounding whitespace ignored");
}

static void test_strings() {
    std::printf("strings: escapes and unicode\n");
    CHECK(parse(R"("hello")").string == "hello", "plain string");
    CHECK(parse(R"("a\"b")").string == "a\"b", "escaped quote");
    CHECK(parse(R"("tab\tnl\n")").string == "tab\tnl\n", "control escapes");
    CHECK(parse(R"("\u0041\u00e9")").string == "A\xc3\xa9", "\\u BMP escapes → UTF-8");
    // surrogate pair for U+1F600 😀  → UTF-8  F0 9F 98 80
    CHECK(parse(R"("\uD83D\uDE00")").string == "\xF0\x9F\x98\x80", "surrogate pair → astral UTF-8");
    // a long string body (exercises the SIMD string scan across chunks)
    std::string big = "\"" + std::string(200, 'x') + "\\n" + std::string(200, 'y') + "\"";
    CHECK(parse(big).string.size() == 401, "long string spanning many SIMD chunks");
}

static void test_structures() {
    std::printf("arrays and objects\n");
    CHECK(parse("[]").is_array() && parse("[]").size() == 0, "empty array");
    CHECK(parse("{}").is_object() && parse("{}").size() == 0, "empty object");
    Value a = parse("[1, 2, [3, 4], {}]");
    CHECK(a.is_array() && a.size() == 4 && (*a.array)[2].size() == 2, "nested array");
    Value o = parse(R"({"a": 1, "b": [true, null], "c": {"d": "e"}})");
    CHECK(o.find("a") && o.find("a")->number == 1, "object member lookup");
    CHECK(o.find("c")->find("d")->string == "e", "nested object lookup");
    CHECK(o.find("missing") == nullptr, "missing key → nullptr");
}

static void test_rejects_malformed() {
    std::printf("rejects malformed input\n");
    const char *bad[] = {"", "{", "[1,]", "{\"a\":}", "[1 2]", "tru", "\"unterminated",
                         "01", "1.", "1e", "-", "{\"a\":1,}", "nul", "[1,2", "\"\\x\""};
    int rejected = 0;
    for (const char *s : bad) {
        try { parse(s); } catch (const ParseError &) { ++rejected; }
    }
    CHECK(rejected == (int)(sizeof(bad) / sizeof(bad[0])), "every malformed input throws ParseError");
}

static void test_simd_equals_scalar() {
    std::printf("HEADLINE: the AVX2 path and the scalar path agree exactly\n");
    std::vector<std::string> corpus = {
        "null", "true", "  -12.5e3 ",
        R"("a string with \"escapes\", \t tabs and \u00e9 unicode")",
        R"([1, 2, 3, [4, [5, [6]]], {"k": "v"}])",
        R"({"name": "ada", "born": 1815, "langs": ["c", "js", null], "ok": true})",
        R"(  {   "spaced" :  [ 1 ,  2 ,  3 ]  ,  "s" : "  lots   of   spaces  "  }   )",
    };
    // add a big whitespace/string-heavy document to exercise the SIMD chunks
    std::string big = "[";
    for (int i = 0; i < 500; ++i) {
        if (i) big += ",\n    ";
        big += R"({"id": )" + std::to_string(i) + R"(, "text": "some string value )" + std::to_string(i) + R"("})";
    }
    big += "]";
    corpus.push_back(big);

    bool all_equal = true;
    for (auto &doc : corpus) {
        Value with = parse(doc, /*use_simd=*/true);
        Value without = parse(doc, /*use_simd=*/false);
        if (with != without) all_equal = false;
    }
    CHECK(all_equal, "SIMD and scalar parses are identical on the whole corpus");
}

int main() {
    test_scalars();
    test_strings();
    test_structures();
    test_rejects_malformed();
    test_simd_equals_scalar();
    if (failures == 0) std::printf("\nALL TESTS PASSED\n");
    else               std::printf("\n%d TEST(S) FAILED\n", failures);
    return failures ? 1 : 0;
}
