/* deflate.c — LZ77 matching, block type choice, and the encoder.
 *
 * The compressor is two independent decisions:
 *
 *   1. Which matches to emit (lz77_parse below). A hash chain finds candidates;
 *      lazy matching decides whether to take one or hold out for a better match
 *      starting one byte later.
 *   2. How to encode the symbols that result. Three block types are legal and
 *      the encoder emits whichever is smallest, measured rather than guessed.
 *
 * Keeping them separate matters: the decoder cares only about (2), so (1) can
 * be changed freely without touching the format.
 */
#include <stdlib.h>

#include "internal.h"

#define HASH_BITS  15
#define HASH_SIZE  (1u << HASH_BITS)
#define HASH_MASK  (HASH_SIZE - 1)

/* One emitted item: a literal, or a match of (length, distance). */
typedef struct {
    uint16_t length;    /* 0 for a literal */
    uint16_t distance;
    uint8_t  literal;
} token;

typedef struct {
    int good_length;   /* stop lengthening the search once a match is this long */
    int max_chain;     /* how many chain entries to examine */
    int lazy;          /* whether to try a match one byte later */
} level_config;

static const level_config LEVELS[10] = {
    {   0,    0, 0 },  /* 0: stored, never reaches the matcher */
    {   4,    4, 0 },
    {   4,    8, 0 },
    {   4,   32, 0 },
    {   4,   16, 1 },
    {   8,   32, 1 },
    {   8,  128, 1 },  /* 6: default */
    {   8,  256, 1 },
    {  32, 1024, 1 },
    {  32, 4096, 1 },
};

static uint32_t hash3(const uint8_t *p)
{
    /* Three bytes, because MIN_MATCH is 3: a hash over fewer would collide on
     * pairs that cannot be emitted, and over more would miss the shortest
     * legal match. */
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2]) & HASH_MASK;
}

/* Longest match for `pos`, searching the chain. Returns the length, and writes
 * the distance. */
static int find_match(const uint8_t *in, size_t in_len, size_t pos,
                      const int32_t *head, const int32_t *prev,
                      const level_config *cfg, int *out_dist)
{
    size_t max_len = in_len - pos;
    if (max_len > MAX_MATCH) max_len = MAX_MATCH;
    if (max_len < MIN_MATCH) return 0;

    int best_len = 0, best_dist = 0;
    int chain = cfg->max_chain;

    int32_t candidate = head[hash3(in + pos)];
    while (candidate >= 0 && chain-- > 0) {
        /* Nothing longer is representable, so stop. This also guards the
         * lookahead below: at best_len == max_len, a[best_len] is in[in_len],
         * one byte past the input. That read is harmless in practice and is
         * still a buffer overrun — AddressSanitizer finds it immediately. */
        if ((size_t)best_len >= max_len) break;

        size_t distance = pos - (size_t)candidate;
        if (distance == 0 || distance > WINDOW_SIZE) break;

        const uint8_t *a = in + pos, *b = in + candidate;
        /* Check the byte that would extend the current best first. Most
         * candidates fail there, and testing it before the run saves walking
         * the whole prefix only to come up one byte short. Both indices are in
         * bounds: best_len < max_len above, and candidate < pos. */
        if (best_len > 0 && a[best_len] != b[best_len]) {
            candidate = prev[candidate & (WINDOW_SIZE - 1)];
            continue;
        }

        size_t len = 0;
        while (len < max_len && a[len] == b[len]) len++;

        if ((int)len > best_len) {
            best_len = (int)len;
            best_dist = (int)distance;
            /* A match this good is worth stopping for: past `good_length` the
             * chance a longer one is further down the chain does not pay for
             * the walk. Without this the field would be dead configuration. */
            if (best_len >= cfg->good_length) chain >>= 2;
        }
        candidate = prev[candidate & (WINDOW_SIZE - 1)];
    }

    if (best_len >= MIN_MATCH) { *out_dist = best_dist; return best_len; }
    return 0;
}

/* Turn the input into a token stream. Returns the token count, or -1. */
static long lz77_parse(const uint8_t *in, size_t in_len, token *tokens, int level)
{
    const level_config *cfg = &LEVELS[level];

    int32_t *head = malloc(HASH_SIZE * sizeof *head);
    int32_t *prev = malloc(WINDOW_SIZE * sizeof *prev);
    if (!head || !prev) { free(head); free(prev); return -1; }
    for (uint32_t i = 0; i < HASH_SIZE; i++) head[i] = -1;

    long n = 0;
    size_t pos = 0;

    /* Deferred state for lazy matching: a match found at `pos` that has not
     * been committed yet, because the byte after it might start a longer one. */
    int have_prev = 0, prev_len = 0, prev_dist = 0;
    size_t prev_pos = 0;

    while (pos < in_len) {
        int dist = 0, len = 0;
        if (pos + MIN_MATCH <= in_len)
            len = find_match(in, in_len, pos, head, prev, cfg, &dist);

        /* Insert before any decision, so the chain is complete regardless of
         * which branch is taken below. */
        if (pos + MIN_MATCH <= in_len) {
            uint32_t h = hash3(in + pos);
            prev[pos & (WINDOW_SIZE - 1)] = head[h];
            head[h] = (int32_t)pos;
        }

        if (!cfg->lazy) {
            if (len >= MIN_MATCH) {
                tokens[n++] = (token){ (uint16_t)len, (uint16_t)dist, 0 };
                for (size_t k = 1; k < (size_t)len && pos + k + MIN_MATCH <= in_len; k++) {
                    uint32_t h = hash3(in + pos + k);
                    prev[(pos + k) & (WINDOW_SIZE - 1)] = head[h];
                    head[h] = (int32_t)(pos + k);
                }
                pos += len;
            } else {
                tokens[n++] = (token){ 0, 0, in[pos] };
                pos++;
            }
            continue;
        }

        /* Lazy: hold a match back one byte to see if a longer one starts next.
         * The gain is real — a 4-byte match displaced by one byte that becomes
         * a 6-byte match saves more than the literal costs — and it is why
         * levels 4 and up beat 3 on text. */
        if (have_prev) {
            if (len > prev_len) {
                /* The later match wins. Emit the held byte as a literal. */
                tokens[n++] = (token){ 0, 0, in[prev_pos] };
                have_prev = 1; prev_len = len; prev_dist = dist; prev_pos = pos;
                pos++;
            } else {
                tokens[n++] = (token){ (uint16_t)prev_len, (uint16_t)prev_dist, 0 };
                size_t end = prev_pos + prev_len;
                for (size_t k = pos + 1; k < end && k + MIN_MATCH <= in_len; k++) {
                    uint32_t h = hash3(in + k);
                    prev[k & (WINDOW_SIZE - 1)] = head[h];
                    head[h] = (int32_t)k;
                }
                pos = end;
                have_prev = 0; prev_len = 0;
            }
        } else if (len >= MIN_MATCH) {
            have_prev = 1; prev_len = len; prev_dist = dist; prev_pos = pos;
            pos++;
        } else {
            tokens[n++] = (token){ 0, 0, in[pos] };
            pos++;
        }
    }

    if (have_prev)
        tokens[n++] = (token){ (uint16_t)prev_len, (uint16_t)prev_dist, 0 };

    free(head); free(prev);
    return n;
}

/* ---- symbol frequencies -------------------------------------------------- */

static void count_symbols(const token *tokens, long n,
                          uint32_t *lit_freq, uint32_t *dist_freq)
{
    memset(lit_freq, 0, NUM_LITERALS * sizeof *lit_freq);
    memset(dist_freq, 0, NUM_DISTANCES * sizeof *dist_freq);
    for (long i = 0; i < n; i++) {
        if (tokens[i].length == 0) {
            lit_freq[tokens[i].literal]++;
        } else {
            lit_freq[257 + length_to_code(tokens[i].length)]++;
            dist_freq[distance_to_code(tokens[i].distance)]++;
        }
    }
    lit_freq[END_OF_BLOCK]++;
}

/* ---- the fixed code, RFC 1951 §3.2.6 ------------------------------------ */

static void fixed_lengths(uint8_t *lit_len, uint8_t *dist_len)
{
    for (int i = 0;   i < 144; i++) lit_len[i] = 8;
    for (int i = 144; i < 256; i++) lit_len[i] = 9;
    for (int i = 256; i < 280; i++) lit_len[i] = 7;
    for (int i = 280; i < NUM_LITERALS; i++) lit_len[i] = 8;
    for (int i = 0; i < NUM_DISTANCES; i++) dist_len[i] = 5;
}

/* ---- code-length alphabet ------------------------------------------------ */

/* Pack lit/len and distance lengths into the RLE'd code-length symbol stream
 * that a dynamic block header carries. Returns the symbol count; `extra`
 * receives the extra-bits value that accompanies symbols 16/17/18. */
static int pack_code_lengths(const uint8_t *lengths, int n,
                             uint8_t *syms, uint8_t *extra)
{
    int out = 0, i = 0;
    while (i < n) {
        int len = lengths[i], run = 1;
        while (i + run < n && lengths[i + run] == len) run++;

        if (len == 0) {
            while (run >= 11) {
                int take = run > 138 ? 138 : run;
                syms[out] = 18; extra[out++] = (uint8_t)(take - 11);
                run -= take; i += take;
            }
            while (run >= 3) {
                int take = run > 10 ? 10 : run;
                syms[out] = 17; extra[out++] = (uint8_t)(take - 3);
                run -= take; i += take;
            }
            while (run-- > 0) { syms[out] = 0; extra[out++] = 0; i++; }
        } else {
            /* Symbol 16 repeats the *previous* length, so the first one has to
             * be written literally before any repeat can refer to it. */
            syms[out] = (uint8_t)len; extra[out++] = 0; i++; run--;
            while (run >= 3) {
                int take = run > 6 ? 6 : run;
                syms[out] = 16; extra[out++] = (uint8_t)(take - 3);
                run -= take; i += take;
            }
            while (run-- > 0) { syms[out] = (uint8_t)len; extra[out++] = 0; i++; }
        }
    }
    return out;
}

/* ---- cost estimation ----------------------------------------------------- */

static size_t token_bits(const token *tokens, long n,
                         const uint8_t *lit_len, const uint8_t *dist_len)
{
    size_t bits = 0;
    for (long i = 0; i < n; i++) {
        if (tokens[i].length == 0) {
            bits += lit_len[tokens[i].literal];
        } else {
            int lc = length_to_code(tokens[i].length);
            int dc = distance_to_code(tokens[i].distance);
            bits += lit_len[257 + lc] + LENGTH_EXTRA[lc];
            bits += dist_len[dc] + DIST_EXTRA[dc];
        }
    }
    return bits + lit_len[END_OF_BLOCK];
}

/* ---- emitting ------------------------------------------------------------ */

static void emit_tokens(bitwriter *w, const token *tokens, long n,
                        const uint8_t *lit_len, const uint16_t *lit_code,
                        const uint8_t *dist_len, const uint16_t *dist_code)
{
    for (long i = 0; i < n; i++) {
        if (tokens[i].length == 0) {
            bw_code(w, lit_code[tokens[i].literal], lit_len[tokens[i].literal]);
        } else {
            int lc = length_to_code(tokens[i].length);
            bw_code(w, lit_code[257 + lc], lit_len[257 + lc]);
            if (LENGTH_EXTRA[lc])
                bw_bits(w, (uint32_t)(tokens[i].length - LENGTH_BASE[lc]), LENGTH_EXTRA[lc]);

            int dc = distance_to_code(tokens[i].distance);
            bw_code(w, dist_code[dc], dist_len[dc]);
            if (DIST_EXTRA[dc])
                bw_bits(w, (uint32_t)(tokens[i].distance - DIST_BASE[dc]), DIST_EXTRA[dc]);
        }
    }
    bw_code(w, lit_code[END_OF_BLOCK], lit_len[END_OF_BLOCK]);
}

static void emit_stored(bitwriter *w, const uint8_t *in, size_t len, int final)
{
    /* A stored block still lives in the bit stream, so the header goes out as
     * bits and only then is the stream aligned. LEN and NLEN are whole bytes
     * from that boundary. */
    bw_bits(w, (uint32_t)(final ? 1 : 0), 1);
    bw_bits(w, 0, 2);
    bw_align(w);
    bw_bits(w, (uint32_t)(len & 0xFFFF), 16);
    bw_bits(w, (uint32_t)(~len & 0xFFFF), 16);
    for (size_t i = 0; i < len; i++) bw_bits(w, in[i], 8);
}

size_t deflate_bound(size_t len)
{
    size_t blocks = len / 65535 + 1;
    return len + blocks * 5 + 8;
}

long deflate_compress(const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, int level)
{
    if (level < 0 || level > 9) level = DF_LEVEL_DEFAULT;

    bitwriter w;
    bw_init(&w, out, out_cap);

    if (in_len == 0) {
        /* An empty input still needs a well-formed stream: a final empty
         * stored block. Emitting nothing at all produces a file that every
         * decoder reports as truncated. */
        emit_stored(&w, in, 0, 1);
        size_t written = bw_finish(&w);
        return w.overflow ? DF_ERR_OUTPUT : (long)written;
    }

    if (level == DF_LEVEL_STORE) {
        size_t pos = 0;
        while (pos < in_len) {
            size_t chunk = in_len - pos > 65535 ? 65535 : in_len - pos;
            emit_stored(&w, in + pos, chunk, pos + chunk >= in_len);
            pos += chunk;
        }
        size_t written = bw_finish(&w);
        return w.overflow ? DF_ERR_OUTPUT : (long)written;
    }

    token *tokens = malloc((in_len + 1) * sizeof *tokens);
    if (!tokens) return DF_ERR_MEMORY;

    long n = lz77_parse(in, in_len, tokens, level);
    if (n < 0) { free(tokens); return DF_ERR_MEMORY; }

    uint32_t lit_freq[NUM_LITERALS], dist_freq[NUM_DISTANCES];
    count_symbols(tokens, n, lit_freq, dist_freq);

    /* --- candidate 1: fixed codes --- */
    uint8_t  fix_lit_len[NUM_LITERALS], fix_dist_len[NUM_DISTANCES];
    uint16_t fix_lit_code[NUM_LITERALS], fix_dist_code[NUM_DISTANCES];
    fixed_lengths(fix_lit_len, fix_dist_len);
    huff_assign_codes(fix_lit_len, NUM_LITERALS, fix_lit_code);
    huff_assign_codes(fix_dist_len, NUM_DISTANCES, fix_dist_code);
    size_t fixed_bits = 3 + token_bits(tokens, n, fix_lit_len, fix_dist_len);

    /* --- candidate 2: dynamic codes --- */
    uint8_t  dyn_lit_len[NUM_LITERALS], dyn_dist_len[NUM_DISTANCES];
    uint16_t dyn_lit_code[NUM_LITERALS], dyn_dist_code[NUM_DISTANCES];
    huff_build_lengths(lit_freq, NUM_LITERALS, MAX_CODE_BITS, dyn_lit_len);
    huff_build_lengths(dist_freq, NUM_DISTANCES, MAX_CODE_BITS, dyn_dist_len);

    /* At least one distance code must exist even when the block is all
     * literals: HDIST is transmitted as "count minus one", so zero is not
     * expressible. One unused 1-bit code is the conventional answer. */
    int any_dist = 0;
    for (int i = 0; i < NUM_DISTANCES; i++) if (dyn_dist_len[i]) any_dist = 1;
    if (!any_dist) dyn_dist_len[0] = 1;

    huff_assign_codes(dyn_lit_len, NUM_LITERALS, dyn_lit_code);
    huff_assign_codes(dyn_dist_len, NUM_DISTANCES, dyn_dist_code);

    int hlit = NUM_LITERALS;
    while (hlit > 257 && dyn_lit_len[hlit - 1] == 0) hlit--;
    int hdist = NUM_DISTANCES;
    while (hdist > 1 && dyn_dist_len[hdist - 1] == 0) hdist--;

    uint8_t combined[NUM_LITERALS + NUM_DISTANCES];
    memcpy(combined, dyn_lit_len, hlit);
    memcpy(combined + hlit, dyn_dist_len, hdist);

    uint8_t cl_syms[NUM_LITERALS + NUM_DISTANCES];
    uint8_t cl_extra[NUM_LITERALS + NUM_DISTANCES];
    int cl_n = pack_code_lengths(combined, hlit + hdist, cl_syms, cl_extra);

    uint32_t cl_freq[NUM_CODELEN] = {0};
    for (int i = 0; i < cl_n; i++) cl_freq[cl_syms[i]]++;
    uint8_t  cl_len[NUM_CODELEN];
    uint16_t cl_code[NUM_CODELEN];
    huff_build_lengths(cl_freq, NUM_CODELEN, MAX_CL_BITS, cl_len);
    huff_assign_codes(cl_len, NUM_CODELEN, cl_code);

    int hclen = NUM_CODELEN;
    while (hclen > 4 && cl_len[CODELEN_ORDER[hclen - 1]] == 0) hclen--;

    size_t header_bits = 3 + 5 + 5 + 4 + (size_t)hclen * 3;
    for (int i = 0; i < cl_n; i++) {
        header_bits += cl_len[cl_syms[i]];
        if (cl_syms[i] == 16) header_bits += 2;
        else if (cl_syms[i] == 17) header_bits += 3;
        else if (cl_syms[i] == 18) header_bits += 7;
    }
    size_t dynamic_bits = header_bits + token_bits(tokens, n, dyn_lit_len, dyn_dist_len);

    /* --- candidate 3: stored --- */
    size_t stored_bits = 3 + 32 + in_len * 8;

    /* Measured, not guessed. On short or already-compressed input the stored
     * block genuinely wins, and an encoder that never checks produces files
     * larger than the input — the one outcome a compressor must not have. */
    if (stored_bits <= fixed_bits && stored_bits <= dynamic_bits) {
        free(tokens);
        size_t pos = 0;
        while (pos < in_len) {
            size_t chunk = in_len - pos > 65535 ? 65535 : in_len - pos;
            emit_stored(&w, in + pos, chunk, pos + chunk >= in_len);
            pos += chunk;
        }
        size_t written = bw_finish(&w);
        return w.overflow ? DF_ERR_OUTPUT : (long)written;
    }

    if (fixed_bits <= dynamic_bits) {
        bw_bits(&w, 1, 1);            /* BFINAL */
        bw_bits(&w, 1, 2);            /* BTYPE = fixed */
        emit_tokens(&w, tokens, n, fix_lit_len, fix_lit_code,
                    fix_dist_len, fix_dist_code);
    } else {
        bw_bits(&w, 1, 1);            /* BFINAL */
        bw_bits(&w, 2, 2);            /* BTYPE = dynamic */
        bw_bits(&w, (uint32_t)(hlit - 257), 5);
        bw_bits(&w, (uint32_t)(hdist - 1), 5);
        bw_bits(&w, (uint32_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; i++)
            bw_bits(&w, cl_len[CODELEN_ORDER[i]], 3);
        for (int i = 0; i < cl_n; i++) {
            bw_code(&w, cl_code[cl_syms[i]], cl_len[cl_syms[i]]);
            if (cl_syms[i] == 16)      bw_bits(&w, cl_extra[i], 2);
            else if (cl_syms[i] == 17) bw_bits(&w, cl_extra[i], 3);
            else if (cl_syms[i] == 18) bw_bits(&w, cl_extra[i], 7);
        }
        emit_tokens(&w, tokens, n, dyn_lit_len, dyn_lit_code,
                    dyn_dist_len, dyn_dist_code);
    }

    free(tokens);
    size_t written = bw_finish(&w);
    return w.overflow ? DF_ERR_OUTPUT : (long)written;
}
