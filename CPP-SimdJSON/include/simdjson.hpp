// A JSON parser whose byte-scanning hot loops are accelerated with AVX2 SIMD.
//
// The bulk of a JSON parser's time is spent walking bytes: skipping whitespace
// between tokens, and scanning the interiors of strings looking for the closing
// quote or an escape. Both are a perfect fit for SIMD — instead of testing one
// byte per iteration, we load 32 bytes into an AVX2 register, compare all 32
// against the characters of interest at once, and use a movemask to jump
// straight to the first hit. The recursive-descent structure around those loops
// keeps the parser simple and correct; SIMD just makes the scanning faster —
// ~2x on scan-bound input (long strings, generous whitespace); see the benchmark.
//
// A `simd` flag lets the same parser fall back to scalar scanning, which is how
// the tests prove the two paths are equivalent and the benchmark measures the
// speedup.

#ifndef SIMDJSON_HPP
#define SIMDJSON_HPP

#include <charconv>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#define SIMDJSON_HAVE_AVX2 1
#else
#define SIMDJSON_HAVE_AVX2 0
#endif

namespace sj {

// ---------------------------------------------------------------- value (DOM)

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;   // ordered, allows dup keys

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::shared_ptr<Array> array;      // shared_ptr keeps Value small and copyable
    std::shared_ptr<Object> object;

    bool is_null()   const { return type == Type::Null; }
    bool is_bool()   const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array()  const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    const Value *find(std::string_view key) const {
        if (!is_object()) return nullptr;
        for (auto &kv : *object) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    size_t size() const {
        if (is_array()) return array->size();
        if (is_object()) return object->size();
        return 0;
    }

    bool operator==(const Value &o) const {
        if (type != o.type) return false;
        switch (type) {
            case Type::Null:   return true;
            case Type::Bool:   return boolean == o.boolean;
            case Type::Number: return number == o.number;
            case Type::String: return string == o.string;
            case Type::Array:  return *array == *o.array;
            case Type::Object: return *object == *o.object;
        }
        return false;
    }
    bool operator!=(const Value &o) const { return !(*this == o); }
};

struct ParseError : std::runtime_error {
    size_t position;
    ParseError(const std::string &msg, size_t pos)
        : std::runtime_error(msg + " at position " + std::to_string(pos)), position(pos) {}
};

// ---------------------------------------------------------------- parser

class Parser {
    const char *begin_ = nullptr;
    const char *p_ = nullptr;
    const char *end_ = nullptr;
    bool simd_ = true;

public:
    // Parse `input`. If `use_simd` is false, the scalar scanning path is used
    // (identical result, slower) — the tests compare the two.
    Value parse(std::string_view input, bool use_simd = true) {
        begin_ = input.data();
        p_ = input.data();
        end_ = input.data() + input.size();
        simd_ = use_simd;
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (p_ != end_) fail("trailing characters after JSON value");
        return v;
    }

private:
    [[noreturn]] void fail(const char *msg) { throw ParseError(msg, (size_t)(p_ - begin_)); }

    static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    // ---- SIMD-accelerated scanning ----

    void skip_ws() {
#if SIMDJSON_HAVE_AVX2
        if (simd_) {
            while (p_ + 32 <= end_) {
                __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p_));
                __m256i ws = _mm256_or_si256(
                    _mm256_or_si256(_mm256_cmpeq_epi8(chunk, _mm256_set1_epi8(' ')),
                                    _mm256_cmpeq_epi8(chunk, _mm256_set1_epi8('\t'))),
                    _mm256_or_si256(_mm256_cmpeq_epi8(chunk, _mm256_set1_epi8('\n')),
                                    _mm256_cmpeq_epi8(chunk, _mm256_set1_epi8('\r'))));
                uint32_t nonws = ~static_cast<uint32_t>(_mm256_movemask_epi8(ws));
                if (nonws != 0) { p_ += __builtin_ctz(nonws); return; }
                p_ += 32;
            }
        }
#endif
        while (p_ < end_ && is_ws(*p_)) ++p_;
    }

    // advance p_ to the next '"' or '\\' (or end_); returns which char (or 0 at end)
    char scan_to_string_special() {
#if SIMDJSON_HAVE_AVX2
        if (simd_) {
            while (p_ + 32 <= end_) {
                __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p_));
                __m256i hit = _mm256_or_si256(_mm256_cmpeq_epi8(chunk, _mm256_set1_epi8('"')),
                                              _mm256_cmpeq_epi8(chunk, _mm256_set1_epi8('\\')));
                uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(hit));
                if (mask != 0) { p_ += __builtin_ctz(mask); return *p_; }
                p_ += 32;
            }
        }
#endif
        while (p_ < end_ && *p_ != '"' && *p_ != '\\') ++p_;
        return p_ < end_ ? *p_ : '\0';
    }

    // ---- value dispatch ----

    Value parse_value() {
        if (p_ >= end_) fail("unexpected end of input");
        switch (*p_) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return parse_string();
            case 't': { Value v; v.type = Value::Type::Bool; v.boolean = true;  return parse_literal("true",  v); }
            case 'f': { Value v; v.type = Value::Type::Bool; v.boolean = false; return parse_literal("false", v); }
            case 'n': { Value v; v.type = Value::Type::Null; return parse_literal("null", v); }
            default:  return parse_number();
        }
    }

    Value parse_literal(const char *lit, const Value &v) {
        for (const char *l = lit; *l; ++l) {
            if (p_ >= end_ || *p_ != *l) fail("invalid literal");
            ++p_;
        }
        return v;
    }

    Value parse_object() {
        ++p_;  // '{'
        Value v; v.type = Value::Type::Object; v.object = std::make_shared<Object>();
        skip_ws();
        if (p_ < end_ && *p_ == '}') { ++p_; return v; }
        for (;;) {
            skip_ws();
            if (p_ >= end_ || *p_ != '"') fail("expected string key");
            std::string key = std::move(parse_string().string);
            skip_ws();
            if (p_ >= end_ || *p_ != ':') fail("expected ':'");
            ++p_;
            skip_ws();
            v.object->emplace_back(std::move(key), parse_value());
            skip_ws();
            if (p_ >= end_) fail("unterminated object");
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return v; }
            fail("expected ',' or '}'");
        }
    }

    Value parse_array() {
        ++p_;  // '['
        Value v; v.type = Value::Type::Array; v.array = std::make_shared<Array>();
        skip_ws();
        if (p_ < end_ && *p_ == ']') { ++p_; return v; }
        for (;;) {
            skip_ws();
            v.array->push_back(parse_value());
            skip_ws();
            if (p_ >= end_) fail("unterminated array");
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return v; }
            fail("expected ',' or ']'");
        }
    }

    Value parse_string() {
        ++p_;  // opening '"'
        Value v; v.type = Value::Type::String;
        std::string &out = v.string;
        for (;;) {
            const char *start = p_;
            char special = scan_to_string_special();   // SIMD scan of the string body
            out.append(start, (size_t)(p_ - start));    // the run of ordinary characters
            if (special == '\0') fail("unterminated string");
            if (special == '"') { ++p_; return v; }
            // else it's a backslash escape
            ++p_;
            if (p_ >= end_) fail("unterminated escape");
            char e = *p_++;
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u':  append_unicode(out);  break;
                default:   fail("invalid escape");
            }
        }
    }

    unsigned read_hex4() {
        if (end_ - p_ < 4) fail("truncated \\u escape");
        unsigned cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
            else fail("invalid hex digit");
        }
        return cp;
    }

    void append_unicode(std::string &out) {
        unsigned cp = read_hex4();
        if (cp >= 0xD800 && cp <= 0xDBFF) {           // high surrogate → expect a low one
            if (end_ - p_ >= 2 && p_[0] == '\\' && p_[1] == 'u') {
                p_ += 2;
                unsigned lo = read_hex4();
                if (lo >= 0xDC00 && lo <= 0xDFFF)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else fail("invalid low surrogate");
            } else fail("missing low surrogate");
        }
        encode_utf8(cp, out);
    }

    static void encode_utf8(unsigned cp, std::string &out) {
        if (cp <= 0x7F) out.push_back((char)cp);
        else if (cp <= 0x7FF) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    Value parse_number() {
        const char *start = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        // integer part
        if (p_ < end_ && *p_ == '0') ++p_;
        else if (p_ < end_ && *p_ >= '1' && *p_ <= '9') { while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_; }
        else fail("invalid number");
        // fraction
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            if (!(p_ < end_ && *p_ >= '0' && *p_ <= '9')) fail("invalid number (fraction)");
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        // exponent
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) ++p_;
            if (!(p_ < end_ && *p_ >= '0' && *p_ <= '9')) fail("invalid number (exponent)");
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        Value v; v.type = Value::Type::Number;
        auto res = std::from_chars(start, p_, v.number);
        if (res.ec != std::errc() || res.ptr != p_) fail("number out of range");
        return v;
    }
};

// convenience free function
inline Value parse(std::string_view input, bool use_simd = true) {
    return Parser{}.parse(input, use_simd);
}

}  // namespace sj

#endif  // SIMDJSON_HPP
