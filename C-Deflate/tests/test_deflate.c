/* test_deflate.c — self-checking tests for the DEFLATE codec.
 *
 * The suite is arranged so that it can fail in an informative place. Round-trip
 * alone would pass against an encoder and decoder that agreed on the same wrong
 * format, so the properties checked separately are:
 *
 *   - round trip is exact, over shapes chosen to hit each block type;
 *   - a corrupt stream is refused rather than read out of bounds;
 *   - the compressed form is genuinely smaller on compressible input, so a
 *     codec that quietly stored everything would fail;
 *   - randomised round trips, because fixtures only cover what someone thought
 *     of.
 *
 * Whether the output is *really* DEFLATE is not decided here — no self-test can
 * decide that. tools/gzcheck.sh hands it to gunzip.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deflate.h"

static int tests_run = 0, tests_failed = 0;

static void ok(const char *what, int cond)
{
    tests_run++;
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        tests_failed++;
        printf("  FAIL  %s\n", what);
    }
}

/* Deterministic PRNG: a failing random case has to be reproducible, and
 * rand() differs between platforms. */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int round_trip(const uint8_t *data, size_t len, int level, const char *what)
{
    size_t cap = deflate_bound(len) + 64;
    uint8_t *comp = malloc(cap);
    uint8_t *back = malloc(len + 64);
    if (!comp || !back) { free(comp); free(back); return 0; }

    long clen = deflate_compress(data, len, comp, cap, level);
    if (clen < 0) {
        printf("        compress failed: %ld (%s)\n", clen, what);
        free(comp); free(back); return 0;
    }
    long blen = deflate_decompress(comp, (size_t)clen, back, len + 64);
    if (blen < 0) {
        printf("        decompress failed: %ld (%s)\n", blen, what);
        free(comp); free(back); return 0;
    }
    int good = ((size_t)blen == len) && (len == 0 || memcmp(data, back, len) == 0);
    if (!good)
        printf("        mismatch: %zu in, %ld out (%s)\n", len, blen, what);

    free(comp); free(back);
    return good;
}

int main(void)
{
    printf("C-Deflate tests\n\n");

    /* ---- shapes that reach each block type ------------------------------ */

    ok("empty input round-trips", round_trip((const uint8_t *)"", 0, 6, "empty"));

    const char *hello = "hello";
    ok("a five-byte input round-trips",
       round_trip((const uint8_t *)hello, 5, 6, "short"));

    /* Highly repetitive: long matches, dynamic codes. */
    {
        size_t n = 100000;
        uint8_t *buf = malloc(n);
        for (size_t i = 0; i < n; i++) buf[i] = 'a';
        ok("100k identical bytes round-trip", round_trip(buf, n, 6, "aaaa"));

        size_t cap = deflate_bound(n) + 64;
        uint8_t *comp = malloc(cap);
        long clen = deflate_compress(buf, n, comp, cap, 6);
        ok("100k identical bytes compress to under 1% of the input",
           clen > 0 && (size_t)clen < n / 100);
        printf("        100000 bytes -> %ld bytes\n", clen);
        free(comp); free(buf);
    }

    /* English-like text: mixed literals and short matches. */
    {
        const char *para =
            "the quick brown fox jumps over the lazy dog. "
            "the quick brown fox jumps over the lazy dog again, and again, "
            "and the dog does not mind, because the dog is lazy. ";
        size_t plen = strlen(para);
        size_t n = plen * 200;
        uint8_t *buf = malloc(n);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)para[i % plen];
        ok("repeated English text round-trips", round_trip(buf, n, 6, "text"));

        size_t cap = deflate_bound(n) + 64;
        uint8_t *comp = malloc(cap);
        long clen = deflate_compress(buf, n, comp, cap, 6);
        ok("repeated text compresses at least 20x", clen > 0 && (size_t)clen * 20 < n);
        printf("        %zu bytes -> %ld bytes\n", n, clen);
        free(comp); free(buf);
    }

    /* Incompressible: the encoder must not make it bigger. */
    {
        size_t n = 50000;
        uint8_t *buf = malloc(n);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(rng() & 0xFF);
        ok("random bytes round-trip", round_trip(buf, n, 6, "random"));

        size_t cap = deflate_bound(n) + 64;
        uint8_t *comp = malloc(cap);
        long clen = deflate_compress(buf, n, comp, cap, 6);
        /* This is the assertion that a stored block exists and is chosen. An
         * encoder that always emitted Huffman codes would expand this input. */
        ok("random bytes do not expand by more than 0.1%",
           clen > 0 && (size_t)clen <= n + n / 1000 + 16);
        printf("        %zu random bytes -> %ld bytes\n", n, clen);
        free(comp); free(buf);
    }

    /* Overlapping matches: distance 1 repeated, which memcpy would get wrong. */
    {
        uint8_t buf[1000];
        memset(buf, 0, sizeof buf);
        for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i < 3 ? i : 0x5A);
        ok("an overlapping match (distance 1) round-trips",
           round_trip(buf, sizeof buf, 6, "overlap"));
    }

    /* Every level agrees on the output, which is what makes level a tuning
     * knob rather than a format change. */
    {
        const char *para = "abcabcabcabc the the the pattern pattern pattern ";
        size_t plen = strlen(para);
        size_t n = plen * 500;
        uint8_t *buf = malloc(n);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)para[i % plen];
        int all = 1;
        for (int level = 0; level <= 9; level++)
            if (!round_trip(buf, n, level, "levels")) all = 0;
        ok("all ten levels produce a stream that decodes to the input", all);
        free(buf);
    }

    /* ---- lengths across the whole table --------------------------------- */
    {
        /* Match lengths 3..258 all have to encode, and the boundaries between
         * length codes are where an off-by-one in the table shows up. */
        int all = 1;
        for (int len = 3; len <= 258; len++) {
            uint8_t buf[600];
            memset(buf, 'x', sizeof buf);
            for (int i = 0; i < len; i++) buf[i] = (uint8_t)('a' + (i % 26));
            memcpy(buf + 300, buf, (size_t)len);
            if (!round_trip(buf, sizeof buf, 6, "lengths")) { all = 0; break; }
        }
        ok("every match length from 3 to 258 round-trips", all);
    }

    /* ---- corrupt input is refused --------------------------------------- */
    {
        uint8_t out[256];
        /* BTYPE = 3 is reserved and must be rejected, not guessed at. */
        uint8_t bad_type[] = { 0x07 };
        ok("a reserved block type is rejected",
           deflate_decompress(bad_type, sizeof bad_type, out, sizeof out) < 0);

        /* A stored block whose NLEN does not complement LEN. */
        uint8_t bad_nlen[] = { 0x01, 0x05, 0x00, 0x00, 0x00, 'h','e','l','l','o' };
        ok("a stored block with a bad NLEN is rejected",
           deflate_decompress(bad_nlen, sizeof bad_nlen, out, sizeof out) < 0);

        /* Truncation in the middle of a real stream. */
        const char *src = "compress me, compress me, compress me please";
        uint8_t comp[512];
        long clen = deflate_compress((const uint8_t *)src, strlen(src),
                                     comp, sizeof comp, 6);
        ok("a truncated stream is rejected rather than half-decoded",
           clen > 4 && deflate_decompress(comp, (size_t)clen / 2, out, sizeof out) < 0);

        /* Too small an output buffer is an error, not an overflow. */
        ok("an undersized output buffer is reported",
           clen > 0 && deflate_decompress(comp, (size_t)clen, out, 4) < 0);
    }

    /* ---- randomised round trips ----------------------------------------- */
    {
        int all = 1;
        size_t total = 0;
        for (int trial = 0; trial < 400; trial++) {
            size_t n = 1 + (rng() % 20000);
            uint8_t *buf = malloc(n);
            /* Mix of runs, patterns and noise, so the matcher sees all three. */
            int mode = (int)(rng() % 3);
            for (size_t i = 0; i < n; i++) {
                if (mode == 0)      buf[i] = (uint8_t)(rng() & 0xFF);
                else if (mode == 1) buf[i] = (uint8_t)('a' + (rng() % 4));
                else                buf[i] = (uint8_t)((i / 7) % 251);
            }
            if (!round_trip(buf, n, (int)(rng() % 10), "fuzz")) {
                printf("        failing trial %d, %zu bytes, mode %d\n", trial, n, mode);
                all = 0; free(buf); break;
            }
            total += n;
            free(buf);
        }
        ok("400 randomised inputs round-trip exactly", all);
        printf("        %zu bytes through the codec\n", total);
    }

    /* ---- the suite must be able to fail --------------------------------- */
    ok("the suite ran a plausible number of checks", tests_run >= 15);

    printf("\n════════════════════════════════════\n");
    printf("  Tests passed : %d\n", tests_run - tests_failed);
    printf("  Tests failed : %d\n", tests_failed);
    printf("%s\n", tests_failed == 0 ? "  DEFLATE TESTS PASSED" : "  DEFLATE TESTS FAILED");
    printf("════════════════════════════════════\n\n");
    return tests_failed == 0 ? 0 : 1;
}
