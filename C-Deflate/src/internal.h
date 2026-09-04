/* internal.h — shared structures for the encoder and decoder.
 *
 * The two bit orders in DEFLATE are the single most common source of streams
 * that "look right" and that no other implementation will read, so they are
 * stated here once:
 *
 *   - The stream is packed LSB-first. Bit 0 of a byte is consumed first.
 *   - Huffman codes are written MSB-first *within* that stream: the code's
 *     most significant bit goes into the earliest bit position.
 *   - Everything else — the length/distance extra bits, the stored-block
 *     LEN field, HLIT/HDIST/HCLEN — is plain LSB-first.
 *
 * So a Huffman code has to be bit-reversed on the way out and is decoded by
 * walking bits from the LSB end. Nothing else in the format is reversed.
 */
#ifndef DEFLATE_INTERNAL_H
#define DEFLATE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "deflate.h"

/* ---- alphabet sizes, from RFC 1951 §3.2.5 ------------------------------- */
#define NUM_LITERALS   286   /* 0-255 literals, 256 end-of-block, 257-285 lengths */
#define NUM_DISTANCES   30
#define NUM_CODELEN     19
#define MAX_CODE_BITS   15   /* lit/len and distance codes */
#define MAX_CL_BITS      7   /* the code-length alphabet's own codes */

#define END_OF_BLOCK   256

/* ---- LZ77 window -------------------------------------------------------- */
#define WINDOW_BITS    15
#define WINDOW_SIZE    (1u << WINDOW_BITS)   /* 32 KiB, the format's maximum */
#define MIN_MATCH       3
#define MAX_MATCH     258

/* ---- bit writer --------------------------------------------------------- */
typedef struct {
    uint8_t *out;
    size_t   cap;
    size_t   pos;
    uint32_t bit_buf;
    int      bit_count;
    int      overflow;   /* sticky: set once, checked once at the end */
} bitwriter;

void bw_init(bitwriter *w, uint8_t *out, size_t cap);
void bw_bits(bitwriter *w, uint32_t value, int count);   /* LSB-first */
void bw_code(bitwriter *w, uint32_t code, int count);    /* MSB-first (Huffman) */
void bw_align(bitwriter *w);
size_t bw_finish(bitwriter *w);

/* ---- bit reader --------------------------------------------------------- */
typedef struct {
    const uint8_t *in;
    size_t   len;
    size_t   pos;
    uint32_t bit_buf;
    int      bit_count;
    int      underflow;
} bitreader;

void br_init(bitreader *r, const uint8_t *in, size_t len);
uint32_t br_bits(bitreader *r, int count);   /* LSB-first */
void br_align(bitreader *r);

/* ---- canonical Huffman -------------------------------------------------- */
/* A code is fully described by its per-symbol bit lengths: RFC 1951 §3.2.2
 * fixes the assignment from lengths to codes, which is why only lengths are
 * ever transmitted. */
typedef struct {
    uint8_t  lengths[NUM_LITERALS];
    uint16_t codes[NUM_LITERALS];
    int      count;
} huff_enc;

typedef struct {
    /* Counts and symbol table for canonical decoding, per RFC 1951 §3.2.2. */
    uint16_t counts[MAX_CODE_BITS + 1];
    uint16_t symbols[NUM_LITERALS];
} huff_dec;

/* Build lengths minimising total bits, with no code longer than `limit`. */
void huff_build_lengths(const uint32_t *freqs, int n, int limit, uint8_t *lengths);
/* Assign canonical codes to a set of lengths. */
void huff_assign_codes(const uint8_t *lengths, int n, uint16_t *codes);
/* Prepare a decoder from lengths. Returns DF_OK or DF_ERR_CORRUPT. */
int  huff_build_decoder(huff_dec *d, const uint8_t *lengths, int n);
/* Decode one symbol. Returns the symbol, or negative on a bad code. */
int  huff_decode(huff_dec *d, bitreader *r);

/* ---- length and distance code tables ------------------------------------ */
extern const uint16_t LENGTH_BASE[29];
extern const uint8_t  LENGTH_EXTRA[29];
extern const uint16_t DIST_BASE[30];
extern const uint8_t  DIST_EXTRA[30];
extern const uint8_t  CODELEN_ORDER[NUM_CODELEN];

int length_to_code(int length);
int distance_to_code(int distance);

#endif /* DEFLATE_INTERNAL_H */
