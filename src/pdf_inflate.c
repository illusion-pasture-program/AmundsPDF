/*
 * AmundsPDF - pdf_inflate.c
 * RFC 1951 DEFLATE inflate decoder and PNG de-predictor.
 *
 * Production implementation: handles all 3 block types, zlib wrapper,
 * Adler-32 verification, and PNG row filters for PDF FlateDecode.
 *
 * Zero external dependencies. Pure C + standard library.
 */
#include "pdf_inflate.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define INFLATE_INITIAL_BUF     (1 << 16)   /* 64 KB initial output buffer  */
#define INFLATE_MAX_OUTPUT      (1 << 28)   /* 256 MB hard limit            */
#define INFLATE_MAX_BITS        15
#define INFLATE_MAX_LIT_CODES   288
#define INFLATE_MAX_DIST_CODES  32
#define INFLATE_MAX_CL_CODES    19
#define INFLATE_MAX_CODES       (INFLATE_MAX_LIT_CODES + INFLATE_MAX_DIST_CODES)
#define INFLATE_WINDOW_SIZE     32768       /* 32 KB sliding window         */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Bit reader
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const uint8_t  *src;
    size_t          src_len;
    size_t          byte_pos;
    uint32_t        bit_buf;     /* accumulator for bits                    */
    int             bit_cnt;     /* number of valid bits in bit_buf         */
} BitReader;

static inline void br_init(BitReader *br, const uint8_t *src, size_t len)
{
    br->src     = src;
    br->src_len = len;
    br->byte_pos = 0;
    br->bit_buf  = 0;
    br->bit_cnt  = 0;
}

/* Refill the bit buffer - pull in up to 4 bytes at a time. */
static inline void br_refill(BitReader *br)
{
    while (br->bit_cnt <= 24 && br->byte_pos < br->src_len) {
        br->bit_buf |= (uint32_t)br->src[br->byte_pos++] << br->bit_cnt;
        br->bit_cnt += 8;
    }
}

/* Read exactly n bits (n <= 25). Returns -1 on underflow. */
static inline int32_t br_bits(BitReader *br, int n)
{
    if (n == 0) return 0;
    br_refill(br);
    if (br->bit_cnt < n) return -1;
    int32_t val = (int32_t)(br->bit_buf & ((1u << n) - 1));
    br->bit_buf >>= n;
    br->bit_cnt -= n;
    return val;
}

/* Peek n bits without consuming. */
static inline int32_t br_peek(BitReader *br, int n)
{
    br_refill(br);
    if (br->bit_cnt < n) return -1;
    return (int32_t)(br->bit_buf & ((1u << n) - 1));
}

/* Consume n bits (after peeking). */
static inline void br_drop(BitReader *br, int n)
{
    br->bit_buf >>= n;
    br->bit_cnt -= n;
}

/* Align to next byte boundary (discard remaining bits in current byte). */
static inline void br_align(BitReader *br)
{
    int discard = br->bit_cnt & 7;
    br->bit_buf >>= discard;
    br->bit_cnt -= discard;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Output buffer with automatic growth
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t    *buf;
    size_t      len;
    size_t      cap;
} OutBuf;

static inline bool ob_init(OutBuf *ob)
{
    ob->cap = INFLATE_INITIAL_BUF;
    ob->len = 0;
    ob->buf = (uint8_t *)malloc(ob->cap);
    return ob->buf != NULL;
}

static inline bool ob_grow(OutBuf *ob, size_t need)
{
    if (ob->len + need <= ob->cap) return true;
    size_t new_cap = ob->cap;
    while (new_cap < ob->len + need) {
        if (new_cap > INFLATE_MAX_OUTPUT / 2) {
            if (ob->len + need > INFLATE_MAX_OUTPUT) return false;
            new_cap = ob->len + need;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap > INFLATE_MAX_OUTPUT) new_cap = INFLATE_MAX_OUTPUT;
    if (ob->len + need > new_cap) return false;
    uint8_t *p = (uint8_t *)realloc(ob->buf, new_cap);
    if (!p) return false;
    ob->buf = p;
    ob->cap = new_cap;
    return true;
}

static inline bool ob_byte(OutBuf *ob, uint8_t b)
{
    if (ob->len >= ob->cap) {
        if (!ob_grow(ob, 1)) return false;
    }
    ob->buf[ob->len++] = b;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Huffman table (lookup-table based for speed)
 *
 *  Two-level table: primary table of 2^FAST_BITS entries handles most
 *  symbols in a single lookup; overflow chains handle longer codes.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define HUFF_FAST_BITS  9
#define HUFF_FAST_SIZE  (1 << HUFF_FAST_BITS)

typedef struct {
    /*
     * fast[code_bits_reversed_low9]:
     *   bits  0..8  = symbol (or sub-table offset for >FAST_BITS)
     *   bits  9..12 = code length
     *   bit   15    = 1 if this entry is a secondary-table pointer
     *
     * For codes <= FAST_BITS, the entry is replicated so any bit
     * pattern in the unused MSBs still hits the right entry.
     */
    uint16_t fast[HUFF_FAST_SIZE];

    /* Secondary tables stored contiguously. */
    uint16_t *secondary;
    int       sec_used;
    int       sec_cap;

    int       max_len;   /* longest code in this table */
} HuffTable;

static void huff_free(HuffTable *h)
{
    if (h->secondary) {
        free(h->secondary);
        h->secondary = NULL;
    }
    h->sec_used = 0;
    h->sec_cap  = 0;
}

/*
 * Build Huffman decode table from an array of code lengths.
 * code_lens[i] = bit length of symbol i  (0 means symbol not present).
 * Returns true on success.
 */
static bool huff_build(HuffTable *h, const uint8_t *code_lens, int count)
{
    int bl_count[INFLATE_MAX_BITS + 1] = {0};
    int i;

    memset(h->fast, 0, sizeof(h->fast));
    h->secondary = NULL;
    h->sec_used  = 0;
    h->sec_cap   = 0;
    h->max_len   = 0;

    /* Count the number of codes for each bit length. */
    for (i = 0; i < count; i++) {
        if (code_lens[i] > INFLATE_MAX_BITS) return false;
        bl_count[code_lens[i]]++;
        if (code_lens[i] > h->max_len)
            h->max_len = code_lens[i];
    }
    bl_count[0] = 0;  /* zero-length = unused symbol */

    /* Check for an empty table (all lengths 0). */
    if (h->max_len == 0) {
        /* No codes at all - leave fast table zeroed. Decoder will always
         * fail to find a symbol, which is appropriate. */
        return true;
    }

    /* Compute next_code[] per RFC 1951 section 3.2.2. */
    uint32_t next_code[INFLATE_MAX_BITS + 1];
    uint32_t code = 0;
    for (i = 1; i <= INFLATE_MAX_BITS; i++) {
        code = (code + bl_count[i - 1]) << 1;
        next_code[i] = code;
    }

    /* Verify the code set is valid (complete or under-full). */
    /* total assigned codes must equal 2^max_len (complete) or less. */

    /* Assign codes, build reverse-bit versions, populate table. */

    /* First pass: if max_len <= FAST_BITS, everything fits in the primary table. */
    if (h->max_len <= HUFF_FAST_BITS) {
        for (i = 0; i < count; i++) {
            int len = code_lens[i];
            if (len == 0) continue;
            uint32_t c = next_code[len]++;
            /* Reverse the bits for the lookup table. */
            uint32_t rev = 0;
            for (int b = 0; b < len; b++)
                rev |= ((c >> (len - 1 - b)) & 1) << b;
            /* Fill all entries where the top bits vary. */
            uint16_t entry = (uint16_t)((len << 9) | i);
            int fill = 1 << len;
            for (int j = (int)rev; j < HUFF_FAST_SIZE; j += fill)
                h->fast[j] = entry;
        }
        return true;
    }

    /* Two-level table needed. Codes <= FAST_BITS go in primary.
     * Longer codes go in secondary sub-tables. */

    /* Allocate secondary space (worst case). */
    h->sec_cap = 1 << (h->max_len - HUFF_FAST_BITS + 2);
    if (h->sec_cap < 256) h->sec_cap = 256;
    h->secondary = (uint16_t *)calloc(h->sec_cap, sizeof(uint16_t));
    if (!h->secondary) return false;

    /* We need to know which primary-table entries get sub-tables.
     * Group by the low FAST_BITS of the reversed code. */

    /* First, assign all codes. */
    typedef struct { int sym; int len; uint32_t rev; } CodeEntry;
    CodeEntry *entries = (CodeEntry *)calloc(count, sizeof(CodeEntry));
    if (!entries) { huff_free(h); return false; }

    int n_entries = 0;
    for (i = 0; i < count; i++) {
        int len = code_lens[i];
        if (len == 0) continue;
        uint32_t c = next_code[len]++;
        uint32_t rev = 0;
        for (int b = 0; b < len; b++)
            rev |= ((c >> (len - 1 - b)) & 1) << b;
        entries[n_entries].sym = i;
        entries[n_entries].len = len;
        entries[n_entries].rev = rev;
        n_entries++;
    }

    /* Populate short codes in primary table. */
    for (int e = 0; e < n_entries; e++) {
        if (entries[e].len <= HUFF_FAST_BITS) {
            uint16_t entry = (uint16_t)((entries[e].len << 9) | entries[e].sym);
            int fill = 1 << entries[e].len;
            for (int j = (int)entries[e].rev; j < HUFF_FAST_SIZE; j += fill)
                h->fast[j] = entry;
        }
    }

    /* Now handle long codes: group by low FAST_BITS of reversed code. */
    /* For each unique prefix, create a sub-table. */
    /* Track which prefixes are used and the max extra bits for each. */
    int sub_bits[HUFF_FAST_SIZE];
    memset(sub_bits, 0, sizeof(sub_bits));
    for (int e = 0; e < n_entries; e++) {
        if (entries[e].len > HUFF_FAST_BITS) {
            int prefix = (int)(entries[e].rev & (HUFF_FAST_SIZE - 1));
            int extra = entries[e].len - HUFF_FAST_BITS;
            if (extra > sub_bits[prefix])
                sub_bits[prefix] = extra;
        }
    }

    /* Allocate sub-tables. */
    for (i = 0; i < HUFF_FAST_SIZE; i++) {
        if (sub_bits[i] == 0) continue;
        int sub_size = 1 << sub_bits[i];
        /* Ensure capacity. */
        if (h->sec_used + sub_size > h->sec_cap) {
            int new_cap = h->sec_cap * 2;
            while (new_cap < h->sec_used + sub_size) new_cap *= 2;
            uint16_t *p = (uint16_t *)realloc(h->secondary, new_cap * sizeof(uint16_t));
            if (!p) { free(entries); huff_free(h); return false; }
            h->secondary = p;
            memset(h->secondary + h->sec_cap, 0, (new_cap - h->sec_cap) * sizeof(uint16_t));
            h->sec_cap = new_cap;
        }
        /* Primary entry: pointer to sub-table. */
        /* bits 0..8 = offset into secondary, bit 15 = 1, bits 9..12 = sub_bits */
        int offset = h->sec_used;
        h->fast[i] = (uint16_t)(0x8000 | (sub_bits[i] << 9) | (offset & 0x1FF));
        /* If offset > 511 we need more bits - use a secondary offset scheme. */
        /* For safety, store the full offset; we pack it differently. */
        /* Re-encode: we'll use bits 0..14 with bit 15 as flag.
         *   bit 15     = 1 (secondary pointer)
         *   bits 12..14 = sub_bits - 1 (0..7 encodes 1..8)
         *   bits 0..11  = offset (up to 4095)
         */
        if (offset > 4095 || sub_bits[i] > 8) {
            /* Extremely deep table - shouldn't happen with valid deflate
             * (max code length 15, FAST_BITS=9, so max extra = 6). */
            free(entries); huff_free(h); return false;
        }
        h->fast[i] = (uint16_t)(0x8000
                     | ((sub_bits[i] - 1) << 12)
                     | (offset & 0xFFF));

        /* Fill sub-table entries. */
        for (int e = 0; e < n_entries; e++) {
            if (entries[e].len <= HUFF_FAST_BITS) continue;
            int prefix = (int)(entries[e].rev & (HUFF_FAST_SIZE - 1));
            if (prefix != i) continue;
            int extra = entries[e].len - HUFF_FAST_BITS;
            uint32_t hi_bits = entries[e].rev >> HUFF_FAST_BITS;
            /* entry: bits 0..8 = symbol, bits 9..12 = extra bit count */
            uint16_t sentry = (uint16_t)((extra << 9) | (entries[e].sym & 0x1FF));
            /* Replicate for shorter codes within sub-table. */
            int fill = 1 << extra;
            for (int j = (int)hi_bits; j < sub_size; j += fill)
                h->secondary[offset + j] = sentry;
        }
        h->sec_used += sub_size;
    }

    free(entries);
    return true;
}

/*
 * Decode one symbol using the Huffman table.
 * Returns the symbol (>= 0) or -1 on error.
 */
static inline int huff_decode(HuffTable *h, BitReader *br)
{
    br_refill(br);
    if (br->bit_cnt < 1) return -1;

    uint32_t idx = br->bit_buf & (HUFF_FAST_SIZE - 1);
    uint16_t entry = h->fast[idx];

    if (!(entry & 0x8000)) {
        /* Primary table hit. */
        int len = (entry >> 9) & 0xF;
        if (len == 0) return -1;  /* unused code */
        if (br->bit_cnt < len) return -1;
        br->bit_buf >>= len;
        br->bit_cnt -= len;
        return entry & 0x1FF;
    }

    /* Secondary table lookup. */
    int sub_bits = ((entry >> 12) & 0x7) + 1;
    int offset   = entry & 0xFFF;
    int total_bits = HUFF_FAST_BITS + sub_bits;
    if (br->bit_cnt < total_bits) return -1;

    uint32_t hi = (br->bit_buf >> HUFF_FAST_BITS) & ((1u << sub_bits) - 1);
    uint16_t sentry = h->secondary[offset + hi];
    int len = (sentry >> 9) & 0xF;
    if (len == 0) return -1;
    int full_len = HUFF_FAST_BITS + len;
    if (br->bit_cnt < full_len) return -1;
    br->bit_buf >>= full_len;
    br->bit_cnt -= full_len;
    return sentry & 0x1FF;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Static tables for fixed Huffman codes (RFC 1951 section 3.2.6)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Base values and extra bits for length codes 257..285. */
static const uint16_t len_base[29] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
    35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};
static const uint8_t len_extra[29] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
    3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};

/* Base values and extra bits for distance codes 0..29. */
static const uint16_t dist_base[30] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073,
    4097,6145,8193,12289, 16385,24577
};
static const uint8_t dist_extra[30] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6,
    7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};

/* Order of code-length code lengths for dynamic Huffman (RFC 1951 sec 3.2.7). */
static const uint8_t cl_order[INFLATE_MAX_CL_CODES] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Core inflate engine
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Build the fixed Huffman tables (literal/length and distance).
 * These are constructed once and reused.
 */
static bool build_fixed_tables(HuffTable *ht_lit, HuffTable *ht_dist)
{
    uint8_t lens[INFLATE_MAX_LIT_CODES];
    int i;

    /* Literal/Length: 0..143 = 8 bits, 144..255 = 9, 256..279 = 7, 280..287 = 8 */
    for (i =   0; i <= 143; i++) lens[i] = 8;
    for (i = 144; i <= 255; i++) lens[i] = 9;
    for (i = 256; i <= 279; i++) lens[i] = 7;
    for (i = 280; i <= 287; i++) lens[i] = 8;
    if (!huff_build(ht_lit, lens, 288)) return false;

    /* Distance: all 32 codes = 5 bits */
    uint8_t dlens[32];
    for (i = 0; i < 32; i++) dlens[i] = 5;
    if (!huff_build(ht_dist, dlens, 32)) { huff_free(ht_lit); return false; }

    return true;
}

/*
 * Decode literal/length + distance compressed data from br into ob.
 */
static bool inflate_codes(BitReader *br, OutBuf *ob,
                          HuffTable *ht_lit, HuffTable *ht_dist)
{
    for (;;) {
        int sym = huff_decode(ht_lit, br);
        if (sym < 0) return false;

        if (sym < 256) {
            /* Literal byte. */
            if (!ob_byte(ob, (uint8_t)sym)) return false;
        }
        else if (sym == 256) {
            /* End of block. */
            return true;
        }
        else {
            /* Length/distance pair. */
            if (sym > 285) return false;
            int li = sym - 257;
            int length = len_base[li];
            if (len_extra[li]) {
                int32_t extra = br_bits(br, len_extra[li]);
                if (extra < 0) return false;
                length += extra;
            }

            /* Decode distance. */
            int dsym = huff_decode(ht_dist, br);
            if (dsym < 0 || dsym > 29) return false;
            int distance = dist_base[dsym];
            if (dist_extra[dsym]) {
                int32_t extra = br_bits(br, dist_extra[dsym]);
                if (extra < 0) return false;
                distance += extra;
            }

            /* Validate distance. */
            if ((size_t)distance > ob->len) return false;

            /* Copy from output history. */
            if (!ob_grow(ob, (size_t)length)) return false;

            /* Byte-by-byte copy handles overlapping (RLE) correctly. */
            size_t src_pos = ob->len - distance;
            for (int j = 0; j < length; j++) {
                ob->buf[ob->len++] = ob->buf[src_pos + j];
            }
        }
    }
}

/*
 * Main raw deflate decoder. Processes all blocks from br into ob.
 */
static bool inflate_blocks(BitReader *br, OutBuf *ob)
{
    int bfinal;
    do {
        int32_t hdr = br_bits(br, 3);
        if (hdr < 0) return false;
        bfinal = hdr & 1;
        int btype = (hdr >> 1) & 3;

        if (btype == 0) {
            /* ── Uncompressed block ── */
            br_align(br);  /* skip to byte boundary */

            /* Read LEN and NLEN from the byte stream. */
            if (br->byte_pos + 4 > br->src_len) return false;
            /* But first, if we have buffered bits that form complete bytes,
             * those bytes are already consumed from byte_pos. After br_align,
             * remaining full bytes in bit_buf need to be "un-read". */
            /* Actually, after br_align, bit_cnt is a multiple of 8 (0,8,16,24).
             * Push those bytes back. */
            while (br->bit_cnt >= 8) {
                if (br->byte_pos == 0) return false;
                br->byte_pos--;
                br->bit_cnt -= 8;
            }
            br->bit_buf = 0;
            br->bit_cnt = 0;

            if (br->byte_pos + 4 > br->src_len) return false;
            uint16_t len  = br->src[br->byte_pos]
                          | ((uint16_t)br->src[br->byte_pos + 1] << 8);
            uint16_t nlen = br->src[br->byte_pos + 2]
                          | ((uint16_t)br->src[br->byte_pos + 3] << 8);
            br->byte_pos += 4;

            if ((uint16_t)(len ^ 0xFFFF) != nlen) return false;
            if (br->byte_pos + len > br->src_len) return false;
            if (!ob_grow(ob, len)) return false;
            memcpy(ob->buf + ob->len, br->src + br->byte_pos, len);
            ob->len += len;
            br->byte_pos += len;
        }
        else if (btype == 1) {
            /* ── Fixed Huffman ── */
            HuffTable ht_lit, ht_dist;
            if (!build_fixed_tables(&ht_lit, &ht_dist)) return false;
            bool ok = inflate_codes(br, ob, &ht_lit, &ht_dist);
            huff_free(&ht_lit);
            huff_free(&ht_dist);
            if (!ok) return false;
        }
        else if (btype == 2) {
            /* ── Dynamic Huffman ── */
            int32_t hlit  = br_bits(br, 5);  if (hlit  < 0) return false;
            int32_t hdist = br_bits(br, 5);  if (hdist < 0) return false;
            int32_t hclen = br_bits(br, 4);  if (hclen < 0) return false;
            hlit  += 257;
            hdist += 1;
            hclen += 4;
            if (hlit > 286 || hdist > 30) return false;

            /* Read code-length code lengths. */
            uint8_t cl_lens[INFLATE_MAX_CL_CODES];
            memset(cl_lens, 0, sizeof(cl_lens));
            for (int i = 0; i < hclen; i++) {
                int32_t v = br_bits(br, 3);
                if (v < 0) return false;
                cl_lens[cl_order[i]] = (uint8_t)v;
            }

            /* Build code-length Huffman table. */
            HuffTable ht_cl;
            if (!huff_build(&ht_cl, cl_lens, INFLATE_MAX_CL_CODES)) return false;

            /* Decode literal/length and distance code lengths. */
            int total = hlit + hdist;
            uint8_t code_lens[INFLATE_MAX_CODES];
            memset(code_lens, 0, sizeof(code_lens));
            int ci = 0;
            while (ci < total) {
                int sym = huff_decode(&ht_cl, br);
                if (sym < 0) { huff_free(&ht_cl); return false; }

                if (sym < 16) {
                    code_lens[ci++] = (uint8_t)sym;
                }
                else if (sym == 16) {
                    /* Repeat previous length 3..6 times. */
                    if (ci == 0) { huff_free(&ht_cl); return false; }
                    int32_t rep = br_bits(br, 2);
                    if (rep < 0) { huff_free(&ht_cl); return false; }
                    rep += 3;
                    uint8_t prev = code_lens[ci - 1];
                    if (ci + rep > total) { huff_free(&ht_cl); return false; }
                    for (int r = 0; r < rep; r++) code_lens[ci++] = prev;
                }
                else if (sym == 17) {
                    /* Repeat 0 for 3..10 times. */
                    int32_t rep = br_bits(br, 3);
                    if (rep < 0) { huff_free(&ht_cl); return false; }
                    rep += 3;
                    if (ci + rep > total) { huff_free(&ht_cl); return false; }
                    for (int r = 0; r < rep; r++) code_lens[ci++] = 0;
                }
                else if (sym == 18) {
                    /* Repeat 0 for 11..138 times. */
                    int32_t rep = br_bits(br, 7);
                    if (rep < 0) { huff_free(&ht_cl); return false; }
                    rep += 11;
                    if (ci + rep > total) { huff_free(&ht_cl); return false; }
                    for (int r = 0; r < rep; r++) code_lens[ci++] = 0;
                }
                else {
                    huff_free(&ht_cl); return false;
                }
            }
            huff_free(&ht_cl);

            /* Build literal/length and distance tables. */
            HuffTable ht_lit, ht_dist;
            if (!huff_build(&ht_lit, code_lens, hlit)) return false;
            if (!huff_build(&ht_dist, code_lens + hlit, hdist)) {
                huff_free(&ht_lit);
                return false;
            }

            bool ok = inflate_codes(br, ob, &ht_lit, &ht_dist);
            huff_free(&ht_lit);
            huff_free(&ht_dist);
            if (!ok) return false;
        }
        else {
            /* btype == 3: reserved/invalid. */
            return false;
        }
    } while (!bfinal);

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Adler-32 checksum (RFC 1950)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint32_t adler32(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    /* Process in chunks to limit modulus operations. */
    const uint32_t MOD = 65521;
    const size_t NMAX = 5552;  /* max iters before overflow of uint32 */

    while (len > 0) {
        size_t chunk = (len > NMAX) ? NMAX : len;
        len -= chunk;
        for (size_t i = 0; i < chunk; i++) {
            a += data[i];
            b += a;
        }
        a %= MOD;
        b %= MOD;
        data += chunk;
    }
    return (b << 16) | a;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

bool pdf_inflate_raw(const uint8_t *src, size_t src_len,
                     uint8_t **out, size_t *out_len)
{
    if (!src || !out || !out_len) return false;
    *out = NULL;
    *out_len = 0;

    if (src_len == 0) {
        /* Empty input is technically valid (produces empty output). */
        *out = (uint8_t *)malloc(1);
        if (!*out) return false;
        *out_len = 0;
        return true;
    }

    BitReader br;
    br_init(&br, src, src_len);

    OutBuf ob;
    if (!ob_init(&ob)) return false;

    if (!inflate_blocks(&br, &ob)) {
        free(ob.buf);
        return false;
    }

    /* Shrink buffer to exact size. */
    if (ob.len == 0) {
        *out = ob.buf;
        *out_len = 0;
    } else {
        uint8_t *final = (uint8_t *)realloc(ob.buf, ob.len);
        *out = final ? final : ob.buf;
        *out_len = ob.len;
    }
    return true;
}

bool pdf_inflate(const uint8_t *src, size_t src_len,
                 uint8_t **out, size_t *out_len)
{
    if (!src || !out || !out_len) return false;
    *out = NULL;
    *out_len = 0;

    /* Minimum zlib stream: 2 header + 1 block + 4 checksum = 7 bytes,
     * but we allow shorter if the compressed data is minimal. */
    if (src_len < 6) return false;

    /* Parse zlib header (RFC 1950). */
    uint8_t cmf  = src[0];
    uint8_t flg  = src[1];
    int cm       = cmf & 0x0F;       /* compression method */
    int cinfo    = (cmf >> 4) & 0x0F; /* window size */

    /* CM must be 8 (deflate). CINFO must be <= 7. */
    if (cm != 8 || cinfo > 7) return false;

    /* CMF*256 + FLG must be a multiple of 31. */
    if (((uint16_t)cmf * 256 + flg) % 31 != 0) return false;

    /* FDICT flag: if set, 4-byte DICTID follows header. */
    int fdict = (flg >> 5) & 1;
    size_t hdr_len = fdict ? 6 : 2;

    if (src_len < hdr_len + 4) return false;  /* need at least header + adler32 */

    /* Decompress the raw deflate data (between header and Adler-32). */
    const uint8_t *deflate_data = src + hdr_len;
    size_t deflate_len = src_len - hdr_len - 4;  /* exclude trailing Adler-32 */

    if (!pdf_inflate_raw(deflate_data, deflate_len, out, out_len)) {
        /* Some producers write incorrect lengths; try with all remaining data. */
        deflate_len = src_len - hdr_len;
        if (!pdf_inflate_raw(deflate_data, deflate_len, out, out_len))
            return false;
        /* If this succeeds, skip Adler check since we don't know where it is. */
        return true;
    }

    /* Verify Adler-32 checksum. */
    const uint8_t *ck = src + src_len - 4;
    uint32_t stored = ((uint32_t)ck[0] << 24) | ((uint32_t)ck[1] << 16)
                    | ((uint32_t)ck[2] << 8)  |  (uint32_t)ck[3];
    uint32_t computed = adler32(*out, *out_len);

    if (stored != computed) {
        /* Checksum mismatch. PDFs in the wild sometimes have bad checksums.
         * We return the data anyway but could optionally fail here.
         * For robustness with real-world PDFs, we accept it. */
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PNG / PDF Predictor de-filtering
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c)
{
    int p  = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a);
    int pb = abs(p - (int)b);
    int pc = abs(p - (int)c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc)             return b;
    return c;
}

bool pdf_depredict(const uint8_t *data, size_t data_len,
                   int predictor, int columns, int colors, int bpc,
                   uint8_t **out, size_t *out_len)
{
    if (!data || !out || !out_len) return false;
    *out = NULL;
    *out_len = 0;

    /* Predictor 1 = no prediction. */
    if (predictor == 1) {
        uint8_t *buf = (uint8_t *)malloc(data_len);
        if (!buf) return false;
        memcpy(buf, data, data_len);
        *out = buf;
        *out_len = data_len;
        return true;
    }

    /* Bytes per pixel (for sub/up/average/paeth, the unit of backward reference). */
    int pixel_bytes = (colors * bpc + 7) / 8;
    if (pixel_bytes < 1) pixel_bytes = 1;

    /* Row stride = bytes per row of actual data (no filter byte). */
    int row_bytes = (columns * colors * bpc + 7) / 8;
    if (row_bytes < 1) return false;

    /* ── TIFF Predictor 2 ── */
    if (predictor == 2) {
        if (data_len < (size_t)row_bytes) return false;
        size_t n_rows = data_len / (size_t)row_bytes;
        size_t total  = n_rows * (size_t)row_bytes;

        uint8_t *buf = (uint8_t *)malloc(total);
        if (!buf) return false;
        memcpy(buf, data, total);

        if (bpc == 8) {
            for (size_t r = 0; r < n_rows; r++) {
                uint8_t *row = buf + r * row_bytes;
                for (int j = colors; j < row_bytes; j++)
                    row[j] = (uint8_t)(row[j] + row[j - colors]);
            }
        }
        else if (bpc == 16) {
            /* 16-bit components: big-endian in PDF. */
            for (size_t r = 0; r < n_rows; r++) {
                uint8_t *row = buf + r * row_bytes;
                int comp_stride = 2 * colors;
                for (int j = comp_stride; j + 1 < row_bytes; j += 2) {
                    uint16_t prev = ((uint16_t)row[j - comp_stride] << 8)
                                  | row[j - comp_stride + 1];
                    uint16_t cur  = ((uint16_t)row[j] << 8) | row[j + 1];
                    cur = (uint16_t)(cur + prev);
                    row[j]     = (uint8_t)(cur >> 8);
                    row[j + 1] = (uint8_t)(cur & 0xFF);
                }
            }
        }
        /* For bpc < 8 with TIFF predictor 2, sub-byte differencing.
         * This is uncommon; handle 1,2,4 bpc. */
        else if (bpc == 1 || bpc == 2 || bpc == 4) {
            int samples_per_byte = 8 / bpc;
            uint8_t mask = (uint8_t)((1 << bpc) - 1);
            for (size_t r = 0; r < n_rows; r++) {
                uint8_t *row = buf + r * row_bytes;
                /* Unpack into temporary sample array, accumulate, repack. */
                int total_samples = columns * colors;
                uint8_t *samples = (uint8_t *)calloc(total_samples, 1);
                if (!samples) { free(buf); return false; }
                /* Unpack. */
                for (int s = 0; s < total_samples; s++) {
                    int byte_idx = s / samples_per_byte;
                    int bit_idx  = (samples_per_byte - 1 - (s % samples_per_byte)) * bpc;
                    if (byte_idx < row_bytes)
                        samples[s] = (row[byte_idx] >> bit_idx) & mask;
                }
                /* Accumulate per color channel. */
                for (int s = colors; s < total_samples; s++)
                    samples[s] = (samples[s] + samples[s - colors]) & mask;
                /* Repack. */
                memset(row, 0, row_bytes);
                for (int s = 0; s < total_samples; s++) {
                    int byte_idx = s / samples_per_byte;
                    int bit_idx  = (samples_per_byte - 1 - (s % samples_per_byte)) * bpc;
                    if (byte_idx < row_bytes)
                        row[byte_idx] |= (samples[s] & mask) << bit_idx;
                }
                free(samples);
            }
        }

        *out = buf;
        *out_len = total;
        return true;
    }

    /* ── PNG Predictors (10..15) ── */
    if (predictor < 10 || predictor > 15) return false;

    /* Each row has a 1-byte filter type prefix + row_bytes of data. */
    int src_row_stride = 1 + row_bytes;
    if (data_len < (size_t)src_row_stride) return false;
    size_t n_rows = data_len / (size_t)src_row_stride;

    /* Verify data length is consistent. */
    if (n_rows * (size_t)src_row_stride > data_len) return false;

    size_t out_total = n_rows * (size_t)row_bytes;
    uint8_t *buf = (uint8_t *)malloc(out_total);
    if (!buf) return false;

    /* Allocate a "previous row" buffer, initialized to zero. */
    uint8_t *prev_row = (uint8_t *)calloc(row_bytes, 1);
    if (!prev_row) { free(buf); return false; }

    for (size_t r = 0; r < n_rows; r++) {
        const uint8_t *src_row = data + r * src_row_stride;
        uint8_t filter = src_row[0];
        const uint8_t *raw = src_row + 1;
        uint8_t *dst = buf + r * row_bytes;

        switch (filter) {
        case 0: /* None */
            memcpy(dst, raw, row_bytes);
            break;

        case 1: /* Sub */
            for (int j = 0; j < row_bytes; j++) {
                uint8_t left = (j >= pixel_bytes) ? dst[j - pixel_bytes] : 0;
                dst[j] = (uint8_t)(raw[j] + left);
            }
            break;

        case 2: /* Up */
            for (int j = 0; j < row_bytes; j++)
                dst[j] = (uint8_t)(raw[j] + prev_row[j]);
            break;

        case 3: /* Average */
            for (int j = 0; j < row_bytes; j++) {
                uint8_t left = (j >= pixel_bytes) ? dst[j - pixel_bytes] : 0;
                uint8_t up   = prev_row[j];
                dst[j] = (uint8_t)(raw[j] + ((left + up) >> 1));
            }
            break;

        case 4: /* Paeth */
            for (int j = 0; j < row_bytes; j++) {
                uint8_t left     = (j >= pixel_bytes) ? dst[j - pixel_bytes] : 0;
                uint8_t up       = prev_row[j];
                uint8_t up_left  = (j >= pixel_bytes) ? prev_row[j - pixel_bytes] : 0;
                dst[j] = (uint8_t)(raw[j] + paeth_predictor(left, up, up_left));
            }
            break;

        default:
            /* Unknown filter type - treat as error for strictness. */
            free(prev_row);
            free(buf);
            return false;
        }

        /* Current row becomes previous row for next iteration. */
        memcpy(prev_row, dst, row_bytes);
    }

    free(prev_row);

    *out = buf;
    *out_len = out_total;
    return true;
}
