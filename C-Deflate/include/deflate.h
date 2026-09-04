/* deflate.h — DEFLATE (RFC 1951) compression and decompression.
 *
 * Two entry points that are inverses of each other, plus a gzip wrapper so the
 * output can be handed to a program that was not written here.
 *
 * Buffers are caller-owned. Every function returns the number of bytes written,
 * or a negative DF_ error code; none of them allocate on the caller's behalf
 * except deflate_bound(), which only computes a size.
 */
#ifndef DEFLATE_H
#define DEFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Errors are values, never crashes: a truncated or corrupt stream is an
 * ordinary return, because the caller is usually mid-way through a file it
 * still wants to report on. */
#define DF_OK             0
#define DF_ERR_OUTPUT    (-1)   /* output buffer too small */
#define DF_ERR_INPUT     (-2)   /* truncated input */
#define DF_ERR_CORRUPT   (-3)   /* malformed stream (bad code, bad distance) */
#define DF_ERR_MEMORY    (-4)   /* allocation failed */

/* Compression levels. Level 0 stores; 1-9 differ only in how hard the matcher
 * looks, never in what the decoder must do. */
#define DF_LEVEL_STORE    0
#define DF_LEVEL_FAST     1
#define DF_LEVEL_DEFAULT  6
#define DF_LEVEL_MAX      9

/* Worst case output for `len` bytes: a stored stream is the input plus a 5-byte
 * header per 65535-byte block, plus one for the final short block. */
size_t deflate_bound(size_t len);

/* Raw DEFLATE, as RFC 1951 defines it: no zlib header, no gzip wrapper.
 * Returns bytes written, or a negative DF_ error. */
long deflate_compress(const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, int level);

/* The inverse. Returns bytes written, or a negative DF_ error. */
long deflate_decompress(const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_cap);

/* The same stream inside a gzip container (RFC 1952): 10-byte header, the
 * DEFLATE data, then CRC-32 and the uncompressed length, both little-endian.
 * This exists so `gunzip` can be the judge of whether the encoder is right. */
long gzip_compress(const uint8_t *in, size_t in_len,
                   uint8_t *out, size_t out_cap, int level);

/* CRC-32 as gzip and PNG use it (reflected, polynomial 0xEDB88320). */
uint32_t deflate_crc32(const uint8_t *data, size_t len);

#endif /* DEFLATE_H */
