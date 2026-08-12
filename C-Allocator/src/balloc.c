/* balloc — segregated explicit-free-list allocator with boundary-tag coalescing.
 *
 * Layout of one block (sizes are bytes; a word is 8 bytes on LP64):
 *
 *     +--------+-----------------------------+--------+
 *     | header |        payload  ...         | footer |
 *     +--------+-----------------------------+--------+
 *
 * The header and footer each hold the block's total size with the low bit used
 * as the allocated flag. Because the footer duplicates the size, freeing a block
 * can read the *previous* block's footer (the word just before this header) to
 * learn its size and coalesce with it — the boundary-tag trick.
 *
 * Free blocks additionally store two pointers (next/prev) at the start of their
 * payload, threading them onto one of NCLASS segregated free lists chosen by
 * size. Allocation scans the smallest adequate class first.
 *
 * Memory comes from the OS in mmap'd chunks. Each chunk is framed by a one-word
 * allocated "prologue" footer and an allocated zero-size "epilogue" header, so
 * coalescing never walks past the ends of a chunk. */

#include "balloc.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ------------------------------------------------------------------ layout */

typedef uint64_t word_t;

#define WSIZE      ((size_t)8)                 /* header/footer size */
#define DSIZE      ((size_t)16)                /* alignment */
#define MIN_BLOCK  ((size_t)32)                /* hdr+ftr + two free-list ptrs */
#define CHUNK      ((size_t)(1 << 20))         /* default OS request: 1 MiB */
#define NCLASS     16

#define ALIGN_UP(n, a)  (((n) + (a) - 1) & ~((a) - 1))
#define PACK(size, alloc) ((size) | (alloc))
#define GET(p)        (*(word_t *)(p))
#define PUT(p, v)     (*(word_t *)(p) = (word_t)(v))
#define BLK_SIZE(p)   (GET(p) & ~(word_t)0x7)
#define BLK_ALLOC(p)  (GET(p) & 0x1)

/* bp points at the payload. */
#define HDRP(bp)      ((char *)(bp) - WSIZE)
#define FTRP(bp)      ((char *)(bp) + BLK_SIZE(HDRP(bp)) - DSIZE)
#define NEXT_BLKP(bp) ((char *)(bp) + BLK_SIZE(HDRP(bp)))
#define PREV_BLKP(bp) ((char *)(bp) - BLK_SIZE((char *)(bp) - DSIZE))

/* Free blocks store these two pointers at the front of the payload. */
typedef struct fnode {
    struct fnode *next;
    struct fnode *prev;
} fnode;

/* One mmap region. The header lives at the very start of the region; the usable
 * heap begins after it, aligned so that the first payload is 16-byte aligned. */
typedef struct chunk {
    struct chunk *next;
    size_t        size;       /* total mmap size, including this header */
} chunk;

static fnode  *g_class[NCLASS];
static chunk  *g_chunks;
static balloc_stats g_stats;

/* ------------------------------------------------------- size-class helpers */

static int size_class(size_t size) {
    int c = 0;
    size >>= 5;                 /* MIN_BLOCK == 32 maps to class 0 */
    while (size && c < NCLASS - 1) { size >>= 1; c++; }
    return c;
}

static void list_insert(fnode *n, size_t size) {
    int c = size_class(size);
    n->prev = NULL;
    n->next = g_class[c];
    if (g_class[c]) g_class[c]->prev = n;
    g_class[c] = n;
}

static void list_remove(fnode *n, size_t size) {
    int c = size_class(size);
    if (n->prev) n->prev->next = n->next;
    else         g_class[c] = n->next;
    if (n->next) n->next->prev = n->prev;
}

/* ------------------------------------------------------------- coalescing */

/* Merge bp with any free physical neighbours, fix boundary tags, and return the
 * (possibly larger, still free and NOT yet on a list) coalesced payload ptr. */
/* Detach a free neighbour that is being absorbed, updating the free-block
 * accounting (the caller re-adds the merged block afterwards). */
static void absorb(void *nb) {
    size_t nsize = BLK_SIZE(HDRP(nb));
    list_remove((fnode *)nb, nsize);
    g_stats.blocks_free--;
    g_stats.bytes_free -= nsize - DSIZE;
}

static void *coalesce(void *bp) {
    size_t prev_alloc = BLK_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = BLK_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = BLK_SIZE(HDRP(bp));

    if (prev_alloc && next_alloc) {
        return bp;
    } else if (prev_alloc && !next_alloc) {
        void *nb = NEXT_BLKP(bp);
        absorb(nb);
        size += BLK_SIZE(HDRP(nb));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    } else if (!prev_alloc && next_alloc) {
        void *pb = PREV_BLKP(bp);
        absorb(pb);
        size += BLK_SIZE(HDRP(pb));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(pb), PACK(size, 0));
        bp = pb;
    } else {
        void *pb = PREV_BLKP(bp), *nb = NEXT_BLKP(bp);
        absorb(pb);
        absorb(nb);
        size += BLK_SIZE(HDRP(pb)) + BLK_SIZE(HDRP(nb));
        PUT(HDRP(pb), PACK(size, 0));
        PUT(FTRP(nb), PACK(size, 0));
        bp = pb;
    }
    return bp;
}

/* ------------------------------------------------------------- grow heap */

/* mmap a fresh chunk of at least `need` payload-usable bytes, frame it with a
 * prologue/epilogue, and return its single big free block (already listed). */
static void *grow(size_t need) {
    size_t total = ALIGN_UP(need + sizeof(chunk) + 3 * WSIZE + DSIZE,
                            CHUNK > need ? CHUNK : ALIGN_UP(need, 4096));
    void *region = mmap(NULL, total, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) return NULL;

    chunk *ch = (chunk *)region;
    ch->size = total;
    ch->next = g_chunks;
    g_chunks = ch;
    g_stats.chunks++;
    g_stats.heap_bytes += total;

    /* Lay out: [chunk hdr][pad to align payload][prologue ftr][free block ...]
     * [epilogue hdr]. We want the first *payload* 16-byte aligned. The free
     * block header sits one word before its payload. */
    char *p = (char *)region + sizeof(chunk);
    /* prologue: a single allocated footer word so PREV of the first block is
     * "allocated"; place it so the following block's payload is aligned. */
    char *first_payload = (char *)ALIGN_UP((uintptr_t)(p + WSIZE + WSIZE), DSIZE);
    char *prologue_ftr = first_payload - WSIZE - WSIZE; /* header of block is at first_payload-WSIZE */
    PUT(prologue_ftr, PACK(WSIZE, 1));

    char *fh = first_payload - WSIZE;                    /* free block header */
    char *epilogue = (char *)region + total - WSIZE;     /* last word */
    size_t bsize = (size_t)(epilogue - fh);              /* header..just before epilogue */
    bsize &= ~(size_t)0x7;
    PUT(fh, PACK(bsize, 0));
    PUT(fh + bsize - WSIZE, PACK(bsize, 0));             /* footer */
    PUT(epilogue, PACK(0, 1));                            /* epilogue: alloc, size 0 */

    void *bp = fh + WSIZE;
    list_insert((fnode *)bp, bsize);
    g_stats.bytes_free += bsize - DSIZE;
    g_stats.blocks_free++;
    return bp;
}

/* ------------------------------------------------------------- place / split */

static void *find_fit(size_t asize) {
    for (int c = size_class(asize); c < NCLASS; c++) {
        for (fnode *n = g_class[c]; n; n = n->next)
            if (BLK_SIZE(HDRP(n)) >= asize) return n;
    }
    return NULL;
}

/* Allocate asize out of the free block bp, splitting the remainder off if it is
 * big enough to be its own block. Returns the payload pointer. */
static void *place(void *bp, size_t asize) {
    size_t csize = BLK_SIZE(HDRP(bp));
    list_remove((fnode *)bp, csize);
    g_stats.bytes_free -= csize - DSIZE;
    g_stats.blocks_free--;

    if (csize - asize >= MIN_BLOCK) {
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        void *rem = NEXT_BLKP(bp);
        size_t rsize = csize - asize;
        PUT(HDRP(rem), PACK(rsize, 0));
        PUT(FTRP(rem), PACK(rsize, 0));
        list_insert((fnode *)rem, rsize);
        g_stats.bytes_free += rsize - DSIZE;
        g_stats.blocks_free++;
    } else {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
    g_stats.bytes_in_use += BLK_SIZE(HDRP(bp)) - DSIZE;
    g_stats.blocks_in_use++;
    return bp;
}

/* ------------------------------------------------------------- public API */

void *balloc(size_t size) {
    if (size == 0) return NULL;
    size_t asize = ALIGN_UP(size + DSIZE, DSIZE);   /* + header + footer, aligned */
    if (asize < MIN_BLOCK) asize = MIN_BLOCK;

    void *bp = find_fit(asize);
    if (!bp) {
        bp = grow(asize);
        if (!bp) return NULL;
    }
    return place(bp, asize);
}

void bfree(void *ptr) {
    if (!ptr) return;
    size_t size = BLK_SIZE(HDRP(ptr));
    g_stats.bytes_in_use -= size - DSIZE;
    g_stats.blocks_in_use--;
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));
    void *bp = coalesce(ptr);
    list_insert((fnode *)bp, BLK_SIZE(HDRP(bp)));
    g_stats.bytes_free += BLK_SIZE(HDRP(bp)) - DSIZE;
    g_stats.blocks_free++;
}

void *bcalloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) return NULL;   /* overflow */
    size_t total = nmemb * size;
    void *p = balloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *brealloc(void *ptr, size_t size) {
    if (!ptr) return balloc(size);
    if (size == 0) { bfree(ptr); return NULL; }
    size_t old_payload = BLK_SIZE(HDRP(ptr)) - DSIZE;
    if (size <= old_payload) return ptr;                    /* shrink: keep block */
    void *np = balloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, old_payload);
    bfree(ptr);
    return np;
}

void balloc_stats_get(balloc_stats *out) { *out = g_stats; }

void balloc_reset(void) {
    chunk *c = g_chunks;
    while (c) {
        chunk *n = c->next;
        size_t sz = c->size;
        munmap(c, sz);
        c = n;
    }
    g_chunks = NULL;
    memset(g_class, 0, sizeof(g_class));
    memset(&g_stats, 0, sizeof(g_stats));
}

/* ------------------------------------------------------------- invariants */

static int on_free_list(void *bp) {
    for (int c = 0; c < NCLASS; c++)
        for (fnode *n = g_class[c]; n; n = n->next)
            if ((void *)n == bp) return 1;
    return 0;
}

int balloc_check(void) {
    for (chunk *ch = g_chunks; ch; ch = ch->next) {
        char *region = (char *)ch;
        char *p = region + sizeof(chunk);
        char *first_payload = (char *)ALIGN_UP((uintptr_t)(p + WSIZE + WSIZE), DSIZE);
        void *bp = first_payload;
        void *prev = NULL;
        while (BLK_SIZE(HDRP(bp)) > 0) {                     /* until epilogue */
            if ((uintptr_t)bp % DSIZE != 0) return -1;       /* payload alignment */
            if (GET(HDRP(bp)) != GET(FTRP(bp))) return -2;   /* header == footer */
            size_t sz = BLK_SIZE(HDRP(bp));
            if (sz < MIN_BLOCK) return -3;                   /* minimum size */
            if (!BLK_ALLOC(HDRP(bp))) {                      /* free block checks */
                if (!on_free_list(bp)) return -4;            /* must be listed */
                if (prev && !BLK_ALLOC(HDRP(prev)))
                    return -5;                               /* no adjacent free pair */
            }
            prev = bp;
            bp = NEXT_BLKP(bp);
        }
    }
    /* every listed node must actually be a free block */
    for (int c = 0; c < NCLASS; c++)
        for (fnode *n = g_class[c]; n; n = n->next)
            if (BLK_ALLOC(HDRP(n))) return -6;
    return 0;
}
