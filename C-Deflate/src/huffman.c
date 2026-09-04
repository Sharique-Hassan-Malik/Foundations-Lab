/* huffman.c — canonical Huffman codes: lengths in, codes out.
 *
 * DEFLATE transmits only the bit *length* per symbol, because RFC 1951 §3.2.2
 * fixes how lengths become codes. That rule is the whole of huff_assign_codes,
 * and running it in reverse is the whole of the decoder. Nothing here invents a
 * tree layout, so an encoder and decoder written from the same paragraph agree
 * by construction.
 */
#include "internal.h"

/* ---- building lengths ---------------------------------------------------- */

typedef struct { uint32_t freq; int left, right; } node;

/* Package-merge is the textbook way to build length-limited codes. This uses
 * the cheaper standard trick instead: build an unlimited Huffman tree and, if
 * anything exceeds the limit, halve every frequency and rebuild.
 *
 * Halving strictly reduces the spread between the largest and smallest
 * frequency, and the depth of a Huffman tree is bounded by that spread, so the
 * loop terminates. The cost is a code that can be a fraction of a percent
 * larger than optimal on pathological inputs — paid only when the limit would
 * otherwise be exceeded, which real data essentially never does. It is a
 * deliberate trade of a little ratio for a lot less code.
 */
static int build_once(const uint32_t *freqs, int n, uint8_t *lengths)
{
    node nodes[2 * NUM_LITERALS];
    int heap[NUM_LITERALS + 1];
    int heap_len = 0, node_count = 0;

    for (int i = 0; i < n; i++) lengths[i] = 0;

    for (int i = 0; i < n; i++) {
        if (freqs[i] == 0) continue;
        nodes[node_count] = (node){ freqs[i], -1, -1 };
        heap[heap_len++] = node_count++;
    }

    /* Fewer than two used symbols cannot form a tree, and a zero-length code is
     * not representable. Give the one (or zero) used symbol a 1-bit code: the
     * format allows an incomplete code here and every decoder accepts it. */
    if (heap_len == 0) return 0;
    if (heap_len == 1) {
        for (int i = 0; i < n; i++) if (freqs[i]) { lengths[i] = 1; break; }
        return 1;
    }

    /* Selection-based extraction. n is at most 286, so the O(n^2) total is a
     * few thousand comparisons — far below the cost of the match search that
     * produced these frequencies. */
    while (heap_len > 1) {
        int a = 0, b = -1;
        for (int i = 1; i < heap_len; i++)
            if (nodes[heap[i]].freq < nodes[heap[a]].freq) a = i;
        int first = heap[a];
        heap[a] = heap[--heap_len];

        b = 0;
        for (int i = 1; i < heap_len; i++)
            if (nodes[heap[i]].freq < nodes[heap[b]].freq) b = i;
        int second = heap[b];
        heap[b] = heap[--heap_len];

        nodes[node_count] = (node){ nodes[first].freq + nodes[second].freq,
                                    first, second };
        heap[heap_len++] = node_count++;
    }

    /* Walk the tree, recording depth. An explicit stack: the depth is bounded
     * by the symbol count, and a recursive walk on 286 symbols is fine but
     * makes the bound implicit. */
    typedef struct { int node; int depth; } walk_item;
    walk_item stack[2 * NUM_LITERALS];
    int sp = 0, max_len = 0;
    stack[sp++] = (walk_item){ heap[0], 0 };

    while (sp > 0) {
        int idx = stack[--sp].node, depth = stack[sp].depth;
        if (nodes[idx].left < 0) {
            /* A leaf. Recover which symbol it was by matching frequency order:
             * leaves were created in symbol order, so index == position. */
            lengths[idx] = (uint8_t)(depth ? depth : 1);
            if (depth > max_len) max_len = depth;
            continue;
        }
        stack[sp++] = (walk_item){ nodes[idx].left,  depth + 1 };
        stack[sp++] = (walk_item){ nodes[idx].right, depth + 1 };
    }
    return max_len ? max_len : 1;
}

void huff_build_lengths(const uint32_t *freqs, int n, int limit, uint8_t *lengths)
{
    uint32_t scaled[NUM_LITERALS];
    uint8_t  leaf_len[2 * NUM_LITERALS];
    int      used[NUM_LITERALS], used_n;

    for (int i = 0; i < n; i++) scaled[i] = freqs[i];

    for (;;) {
        used_n = 0;
        for (int i = 0; i < n; i++) if (scaled[i]) used[used_n++] = i;

        int max_len = build_once(scaled, n, leaf_len);

        /* build_once indexes leaves by creation order, which is the order of
         * the used symbols; map them back onto symbol indices. */
        for (int i = 0; i < n; i++) lengths[i] = 0;
        for (int i = 0; i < used_n; i++) lengths[used[i]] = leaf_len[i];

        if (max_len <= limit) return;

        for (int i = 0; i < n; i++)
            if (scaled[i]) scaled[i] = (scaled[i] + 1) / 2;
    }
}

/* ---- lengths to codes: RFC 1951 §3.2.2 ---------------------------------- */

void huff_assign_codes(const uint8_t *lengths, int n, uint16_t *codes)
{
    uint16_t bl_count[MAX_CODE_BITS + 1] = {0};
    uint16_t next_code[MAX_CODE_BITS + 2] = {0};

    for (int i = 0; i < n; i++) if (lengths[i]) bl_count[lengths[i]]++;

    uint32_t code = 0;
    for (int bits = 1; bits <= MAX_CODE_BITS; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = (uint16_t)code;
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) codes[i] = next_code[lengths[i]]++;
        else            codes[i] = 0;
    }
}

/* ---- decoding ------------------------------------------------------------ */

int huff_build_decoder(huff_dec *d, const uint8_t *lengths, int n)
{
    memset(d->counts, 0, sizeof d->counts);
    for (int i = 0; i < n; i++) {
        if (lengths[i] > MAX_CODE_BITS) return DF_ERR_CORRUPT;
        d->counts[lengths[i]]++;
    }
    d->counts[0] = 0;

    /* Reject an over-subscribed code before it is used. An incomplete one is
     * allowed: a block with a single distance symbol legitimately produces a
     * 1-bit code with one unused branch, and rejecting that would refuse
     * streams other encoders emit. */
    int left = 1;
    for (int bits = 1; bits <= MAX_CODE_BITS; bits++) {
        left <<= 1;
        left -= d->counts[bits];
        if (left < 0) return DF_ERR_CORRUPT;
    }

    uint16_t offsets[MAX_CODE_BITS + 2] = {0};
    for (int bits = 1; bits <= MAX_CODE_BITS; bits++)
        offsets[bits + 1] = offsets[bits] + d->counts[bits];

    for (int i = 0; i < n; i++)
        if (lengths[i]) d->symbols[offsets[lengths[i]]++] = (uint16_t)i;

    return DF_OK;
}

int huff_decode(huff_dec *d, bitreader *r)
{
    int code = 0, first = 0, index = 0;

    /* Canonical decoding: walk one bit at a time, and at each length ask
     * whether the accumulated code falls inside that length's block. This is
     * the assignment rule of huff_assign_codes read backwards, which is why no
     * table has to be shared between the two sides. */
    for (int len = 1; len <= MAX_CODE_BITS; len++) {
        code |= (int)br_bits(r, 1);
        int count = d->counts[len];
        if (code - first < count) return d->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return DF_ERR_CORRUPT;
}
