/* gzdeflate.c — read stdin, write a gzip stream to stdout.
 *
 * Exists so that a program written by someone else can be the judge of whether
 * this encoder emits real DEFLATE. A round-trip test only proves the encoder
 * and decoder agree with each other, which they would even if both were wrong
 * in the same way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deflate.h"

int main(int argc, char **argv)
{
    int level = argc > 1 ? atoi(argv[1]) : 6;

    size_t cap = 1u << 20, len = 0;
    uint8_t *in = malloc(cap);
    if (!in) return 1;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *bigger = realloc(in, cap);
            if (!bigger) { free(in); return 1; }
            in = bigger;
        }
        size_t got = fread(in + len, 1, cap - len, stdin);
        if (got == 0) break;
        len += got;
    }

    size_t out_cap = deflate_bound(len) + 64;
    uint8_t *out = malloc(out_cap);
    if (!out) { free(in); return 1; }

    long n = gzip_compress(in, len, out, out_cap, level);
    if (n < 0) { fprintf(stderr, "gzdeflate: error %ld\n", n); return 1; }

    fwrite(out, 1, (size_t)n, stdout);
    free(in); free(out);
    return 0;
}
