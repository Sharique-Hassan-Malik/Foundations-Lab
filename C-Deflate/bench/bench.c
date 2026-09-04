/* bench.c — ratio and throughput, on inputs with different shapes.
 *
 * Ratio is the number that matters for a compressor and it is measured against
 * a fixed corpus generated here, so the figure does not depend on what happens
 * to be on the machine. Throughput is reported too, because the level knob only
 * means something if the slower levels are actually slower.
 */
/* clock_gettime is POSIX, and -std=c11 hides it without this. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "deflate.h"

static uint32_t rng_state = 0xC0FFEEu;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

typedef struct { const char *name; uint8_t *data; size_t len; } corpus;

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
    size_t n = 1u << 20;   /* 1 MiB of each */
    corpus sets[4];

    sets[0].name = "text-like";
    sets[0].data = malloc(n); sets[0].len = n;
    {
        const char *p = "the quick brown fox jumps over the lazy dog. "
                        "pack my box with five dozen liquor jugs. ";
        size_t plen = strlen(p);
        for (size_t i = 0; i < n; i++) sets[0].data[i] = (uint8_t)p[i % plen];
    }

    sets[1].name = "source-like";
    sets[1].data = malloc(n); sets[1].len = n;
    {
        const char *lines[] = {
            "static int compute(int a, int b) {\n", "    return a + b * 2;\n",
            "}\n\n", "if (x != NULL) {\n", "    free(x);\n", "    x = NULL;\n",
            "}\n", "for (int i = 0; i < count; i++) {\n",
        };
        size_t pos = 0;
        while (pos < n) {
            const char *l = lines[rng() % (sizeof lines / sizeof *lines)];
            size_t len = strlen(l);
            if (pos + len > n) len = n - pos;
            memcpy(sets[1].data + pos, l, len);
            pos += len;
        }
    }

    sets[2].name = "runs";
    sets[2].data = malloc(n); sets[2].len = n;
    {
        size_t pos = 0;
        while (pos < n) {
            uint8_t byte = (uint8_t)(rng() & 0xFF);
            size_t run = 1 + (rng() % 300);
            if (pos + run > n) run = n - pos;
            memset(sets[2].data + pos, byte, run);
            pos += run;
        }
    }

    sets[3].name = "incompressible";
    sets[3].data = malloc(n); sets[3].len = n;
    for (size_t i = 0; i < n; i++) sets[3].data[i] = (uint8_t)(rng() & 0xFF);

    size_t cap = deflate_bound(n) + 64;
    uint8_t *out = malloc(cap);

    printf("\n1 MiB of each, ratio = original / compressed\n\n");
    printf("%-16s %6s %12s %8s %12s\n", "corpus", "level", "compressed", "ratio", "MB/s");
    printf("%-16s %6s %12s %8s %12s\n", "----------------", "-----",
           "------------", "--------", "------------");

    for (int s = 0; s < 4; s++) {
        for (int level = 1; level <= 9; level += 4) {
            double t0 = seconds();
            long clen = deflate_compress(sets[s].data, sets[s].len, out, cap, level);
            double dt = seconds() - t0;
            if (clen < 0) { printf("  error %ld\n", clen); continue; }
            printf("%-16s %6d %12ld %7.2fx %12.1f\n",
                   level == 1 ? sets[s].name : "", level, clen,
                   (double)sets[s].len / (double)clen,
                   (double)sets[s].len / dt / 1e6);
        }
    }

    /* Decompression is one number: it does not have levels. */
    printf("\n");
    long clen = deflate_compress(sets[1].data, sets[1].len, out, cap, 6);
    uint8_t *back = malloc(n + 64);
    double t0 = seconds();
    for (int i = 0; i < 10; i++)
        deflate_decompress(out, (size_t)clen, back, n + 64);
    double dt = (seconds() - t0) / 10.0;
    printf("decompression (source-like): %.1f MB/s\n\n",
           (double)n / dt / 1e6);

    free(back); free(out);
    for (int s = 0; s < 4; s++) free(sets[s].data);
    return 0;
}
