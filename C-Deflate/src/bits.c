/* bits.c — the two bit orders, and the tables that index them.
 *
 * See internal.h for why there are two. bw_bits writes LSB-first, which is
 * every field in the format except a Huffman code; bw_code reverses, which is
 * the Huffman case. Getting these the same way round produces a stream that
 * decodes correctly with your own decoder and with nothing else, which is the
 * failure this file exists to make impossible to write by accident.
 */
#include "internal.h"

/* ---- writer ------------------------------------------------------------- */

void bw_init(bitwriter *w, uint8_t *out, size_t cap)
{
    w->out = out; w->cap = cap; w->pos = 0;
    w->bit_buf = 0; w->bit_count = 0; w->overflow = 0;
}

void bw_bits(bitwriter *w, uint32_t value, int count)
{
    /* Mask rather than trust the caller: an extra-bits field that carried a
     * stray high bit would corrupt every subsequent symbol rather than the one
     * value, and the damage would appear far from its cause. */
    if (count < 32) value &= (1u << count) - 1u;

    w->bit_buf |= value << w->bit_count;
    w->bit_count += count;

    while (w->bit_count >= 8) {
        if (w->pos >= w->cap) { w->overflow = 1; return; }
        w->out[w->pos++] = (uint8_t)(w->bit_buf & 0xFF);
        w->bit_buf >>= 8;
        w->bit_count -= 8;
    }
}

void bw_code(bitwriter *w, uint32_t code, int count)
{
    /* A Huffman code goes out most significant bit first. Reversing here, at
     * the one place codes are written, keeps the reversal out of the tables —
     * where it would have to be undone to compare them against the RFC. */
    uint32_t reversed = 0;
    for (int i = 0; i < count; i++) {
        reversed = (reversed << 1) | ((code >> i) & 1u);
    }
    bw_bits(w, reversed, count);
}

void bw_align(bitwriter *w)
{
    if (w->bit_count > 0) bw_bits(w, 0, 8 - w->bit_count);
}

size_t bw_finish(bitwriter *w)
{
    bw_align(w);
    return w->pos;
}

/* ---- reader ------------------------------------------------------------- */

void br_init(bitreader *r, const uint8_t *in, size_t len)
{
    r->in = in; r->len = len; r->pos = 0;
    r->bit_buf = 0; r->bit_count = 0; r->underflow = 0;
}

uint32_t br_bits(bitreader *r, int count)
{
    while (r->bit_count < count) {
        if (r->pos >= r->len) {
            /* Feed zeros and latch the fact. Returning an error from every bit
             * read would put a branch on the hottest path in the decoder; the
             * flag is checked once per block instead. */
            r->underflow = 1;
            r->bit_buf |= 0u << r->bit_count;
            r->bit_count += 8;
            continue;
        }
        r->bit_buf |= (uint32_t)r->in[r->pos++] << r->bit_count;
        r->bit_count += 8;
    }
    uint32_t value = r->bit_buf & ((count == 32) ? 0xFFFFFFFFu : ((1u << count) - 1u));
    r->bit_buf >>= count;
    r->bit_count -= count;
    return value;
}

void br_align(bitreader *r)
{
    int drop = r->bit_count & 7;
    if (drop) br_bits(r, drop);
}

/* ---- RFC 1951 §3.2.5 tables --------------------------------------------- */

const uint16_t LENGTH_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const uint8_t LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
const uint16_t DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
const uint8_t DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* The code-length alphabet is transmitted in this order so that the lengths
 * most likely to be zero land at the end and can be truncated away. */
const uint8_t CODELEN_ORDER[NUM_CODELEN] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

int length_to_code(int length)
{
    /* Linear from the top: 258 is the special last entry with no extra bits,
     * and a binary search would need the same special case anyway. */
    for (int i = 28; i >= 0; i--) {
        if (length >= LENGTH_BASE[i]) return i;
    }
    return 0;
}

int distance_to_code(int distance)
{
    for (int i = 29; i >= 0; i--) {
        if (distance >= DIST_BASE[i]) return i;
    }
    return 0;
}
