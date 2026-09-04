/* inflate.c — the decoder, and the gzip container.
 *
 * The decoder exists for two reasons beyond symmetry. It makes round-trip
 * testing possible without a third-party library, and it is the only way to
 * check that a *dynamic* block header the encoder wrote can be read back — the
 * header is the part of DEFLATE with the most ways to be subtly wrong, and the
 * encoder cannot catch its own mistakes there.
 */
#include "internal.h"

static int read_dynamic_header(bitreader *r, huff_dec *lit, huff_dec *dist)
{
    int hlit  = (int)br_bits(r, 5) + 257;
    int hdist = (int)br_bits(r, 5) + 1;
    int hclen = (int)br_bits(r, 4) + 4;

    if (hlit > NUM_LITERALS || hdist > NUM_DISTANCES) return DF_ERR_CORRUPT;

    uint8_t cl_len[NUM_CODELEN] = {0};
    for (int i = 0; i < hclen; i++)
        cl_len[CODELEN_ORDER[i]] = (uint8_t)br_bits(r, 3);

    huff_dec cl_dec;
    int rc = huff_build_decoder(&cl_dec, cl_len, NUM_CODELEN);
    if (rc != DF_OK) return rc;

    uint8_t lengths[NUM_LITERALS + NUM_DISTANCES] = {0};
    int n = 0;
    while (n < hlit + hdist) {
        int sym = huff_decode(&cl_dec, r);
        if (sym < 0) return DF_ERR_CORRUPT;

        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            /* Repeat the previous length. With nothing before it there is no
             * previous length to repeat, and a stream that asks is corrupt
             * rather than a special case worth inventing a default for. */
            if (n == 0) return DF_ERR_CORRUPT;
            int repeat = 3 + (int)br_bits(r, 2);
            uint8_t prev = lengths[n - 1];
            while (repeat-- > 0 && n < hlit + hdist) lengths[n++] = prev;
        } else if (sym == 17) {
            int repeat = 3 + (int)br_bits(r, 3);
            while (repeat-- > 0 && n < hlit + hdist) lengths[n++] = 0;
        } else {
            int repeat = 11 + (int)br_bits(r, 7);
            while (repeat-- > 0 && n < hlit + hdist) lengths[n++] = 0;
        }
        if (r->underflow) return DF_ERR_INPUT;
    }

    rc = huff_build_decoder(lit, lengths, hlit);
    if (rc != DF_OK) return rc;
    return huff_build_decoder(dist, lengths + hlit, hdist);
}

static void fixed_decoders(huff_dec *lit, huff_dec *dist)
{
    uint8_t lit_len[NUM_LITERALS], dist_len[NUM_DISTANCES];
    for (int i = 0;   i < 144; i++) lit_len[i] = 8;
    for (int i = 144; i < 256; i++) lit_len[i] = 9;
    for (int i = 256; i < 280; i++) lit_len[i] = 7;
    for (int i = 280; i < NUM_LITERALS; i++) lit_len[i] = 8;
    for (int i = 0; i < NUM_DISTANCES; i++) dist_len[i] = 5;
    huff_build_decoder(lit, lit_len, NUM_LITERALS);
    huff_build_decoder(dist, dist_len, NUM_DISTANCES);
}

long deflate_decompress(const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_cap)
{
    bitreader r;
    br_init(&r, in, in_len);
    size_t written = 0;
    int final = 0;

    while (!final) {
        final = (int)br_bits(&r, 1);
        int type = (int)br_bits(&r, 2);
        if (r.underflow) return DF_ERR_INPUT;

        if (type == 0) {
            br_align(&r);
            /* LEN and NLEN are read through the bit reader so that its byte
             * position stays authoritative; reading them straight from `in`
             * would need the reader's buffered bits accounted for by hand. */
            uint32_t len  = br_bits(&r, 16);
            uint32_t nlen = br_bits(&r, 16);
            if (r.underflow) return DF_ERR_INPUT;
            if ((len & 0xFFFF) != ((~nlen) & 0xFFFF)) return DF_ERR_CORRUPT;
            if (written + len > out_cap) return DF_ERR_OUTPUT;
            for (uint32_t i = 0; i < len; i++) {
                uint32_t byte = br_bits(&r, 8);
                if (r.underflow) return DF_ERR_INPUT;
                out[written++] = (uint8_t)byte;
            }
            continue;
        }

        if (type == 3) return DF_ERR_CORRUPT;

        huff_dec lit, dist;
        if (type == 1) {
            fixed_decoders(&lit, &dist);
        } else {
            int rc = read_dynamic_header(&r, &lit, &dist);
            if (rc != DF_OK) return rc;
        }

        for (;;) {
            int sym = huff_decode(&lit, &r);
            if (sym < 0 || r.underflow) return sym < 0 ? DF_ERR_CORRUPT : DF_ERR_INPUT;
            if (sym == END_OF_BLOCK) break;

            if (sym < 256) {
                if (written >= out_cap) return DF_ERR_OUTPUT;
                out[written++] = (uint8_t)sym;
                continue;
            }

            int lc = sym - 257;
            if (lc >= 29) return DF_ERR_CORRUPT;
            int length = LENGTH_BASE[lc] + (int)br_bits(&r, LENGTH_EXTRA[lc]);

            int dc = huff_decode(&dist, &r);
            if (dc < 0 || dc >= NUM_DISTANCES) return DF_ERR_CORRUPT;
            int distance = DIST_BASE[dc] + (int)br_bits(&r, DIST_EXTRA[dc]);

            /* A distance reaching before the start of the output is the
             * classic way a malformed stream reads memory it should not.
             * Checked here rather than trusted, because the check is one
             * comparison and the alternative is an out-of-bounds read. */
            if ((size_t)distance > written) return DF_ERR_CORRUPT;
            if (written + length > out_cap) return DF_ERR_OUTPUT;

            /* Byte at a time, deliberately: a match may overlap its own output
             * (distance 1, length 20 repeats one byte twenty times), which is
             * legal and common. memcpy would read bytes it has not written. */
            size_t from = written - (size_t)distance;
            for (int i = 0; i < length; i++) out[written + i] = out[from + i];
            written += length;
        }
    }
    return (long)written;
}

/* ---- CRC-32 and the gzip container -------------------------------------- */

uint32_t deflate_crc32(const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static int built = 0;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

long gzip_compress(const uint8_t *in, size_t in_len,
                   uint8_t *out, size_t out_cap, int level)
{
    if (out_cap < 18) return DF_ERR_OUTPUT;

    /* RFC 1952 §2.3. No name, no timestamp: a fixed header makes the output a
     * pure function of the input, so two runs produce identical bytes and the
     * round-trip test can compare them. */
    out[0] = 0x1F; out[1] = 0x8B;   /* magic */
    out[2] = 8;                     /* CM = deflate */
    out[3] = 0;                     /* FLG: nothing set */
    out[4] = out[5] = out[6] = out[7] = 0;   /* MTIME = 0 */
    out[8] = 0;                     /* XFL */
    out[9] = 255;                   /* OS = unknown */

    long body = deflate_compress(in, in_len, out + 10, out_cap - 18, level);
    if (body < 0) return body;

    size_t pos = 10 + (size_t)body;
    uint32_t crc = deflate_crc32(in, in_len);
    uint32_t isize = (uint32_t)(in_len & 0xFFFFFFFFu);

    for (int i = 0; i < 4; i++) out[pos++] = (uint8_t)((crc   >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; i++) out[pos++] = (uint8_t)((isize >> (8 * i)) & 0xFF);
    return (long)pos;
}
