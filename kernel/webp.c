/*
 * webp.c — VP8L (WebP Lossless) decoder.
 *
 * Implements the RIFF/WEBP container parser + the VP8L bitstream decoder per
 * the WebP Lossless Bitstream Specification.
 *
 * Implemented features:
 *   - RIFF/WEBP container: VP8L chunk (lossy VP8 → non-zero graceful reject)
 *   - VP8L bitstream: all 4 transform types (PREDICTOR, COLOR, SUBTRACT_GREEN,
 *     COLOR_INDEXING including bundled small-palette pixels)
 *   - Canonical Huffman trees (simple and complex forms, 19-symbol CLC)
 *   - Meta-Huffman (per-tile Huffman group selection)
 *   - Color cache (hash insert/lookup, up to 2^12 entries)
 *   - LZ77 back-references with the 120-entry distance plane mapping
 *   - ARGB → RGBA byte-order conversion
 *
 * Bitstream order (per spec, §5):
 *   Header → Transforms → Color Cache → Meta-Huffman → Huffman Codes → Pixels
 *
 * Buffers are CALLER-PROVIDED (no malloc).  Every read is bounds-checked;
 * malformed input returns non-zero (fail closed, no UB).
 *
 * Freestanding C — only #include <stdint.h>.  Own memset/memcpy provided.
 */
#include "webp.h"

/* =========================================================================
 * Freestanding helpers
 * ====================================================================== */
static void wbp_memset(void *dst, int c, long n) {
    uint8_t *d = (uint8_t *)dst;
    for (long i = 0; i < n; i++) d[i] = (uint8_t)c;
}
static void wbp_memcpy(void *dst, const void *src, long n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    for (long i = 0; i < n; i++) d[i] = s[i];
}

/* =========================================================================
 * LE helpers
 * ====================================================================== */
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8)
         | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* =========================================================================
 * RIFF/WEBP container
 *
 * Scans the RIFF/WEBP chunk list for "VP8L".  Rejects "VP8 " (lossy).
 * Returns byte offset of VP8L payload on success, -1 on failure.
 * ====================================================================== */
static int find_vp8l(const uint8_t *data, int len, int *out_pl) {
    if (len < 20) return -1;
    if (data[0]!='R'||data[1]!='I'||data[2]!='F'||data[3]!='F') return -1;
    if (data[8]!='W'||data[9]!='E'||data[10]!='B'||data[11]!='P') return -1;
    int p = 12;
    while (p + 8 <= len) {
        uint32_t csz = rd32le(data + p + 4);
        if ((uint64_t)p + 8 + csz > (uint64_t)len) return -1;
        const uint8_t *cc = data + p;
        if (cc[0]=='V'&&cc[1]=='P'&&cc[2]=='8') {
            if (cc[3]==' ') return -1;      /* lossy: reject */
            if (cc[3]=='L') {
                *out_pl = (int)csz;
                return p + 8;
            }
        }
        uint32_t step = 8 + csz + (csz & 1);
        if (step < 8 || (long)p + (long)step > len) return -1;
        p += (int)step;
    }
    return -1;
}

/* =========================================================================
 * LSB-first bit reader
 * ====================================================================== */
typedef struct {
    const uint8_t *src;
    int            len;
    int            pos;
    uint64_t       acc;   /* bit accumulator */
    int            n;     /* valid bits in acc */
    int            err;   /* set on underflow */
} BR;

static void br_init(BR *br, const uint8_t *src, int len) {
    br->src = src; br->len = len; br->pos = 0;
    br->acc = 0; br->n = 0; br->err = 0;
}
static void br_fill(BR *br) {
    while (br->n <= 56 && br->pos < br->len)
        br->acc |= (uint64_t)br->src[br->pos++] << br->n, br->n += 8;
}
static uint32_t br_read(BR *br, int k) {
    if (k == 0) return 0;
    br_fill(br);
    if (br->n < k) { br->err = 1; return 0; }
    uint32_t v = (uint32_t)(br->acc & (((uint64_t)1<<k)-1));
    br->acc >>= k; br->n -= k;
    return v;
}
static int br_ok(const BR *br) { return !br->err; }

/* =========================================================================
 * Compact Huffman tree
 *
 * Up to 2^13 symbols (256+24+color_cache up to 2^12 = 4376).
 * Codes up to 15 bits.  Fast table: 10-bit prefix → (sym, len) in 4 bytes.
 * Overflow: linear scan for codes > 10 bits (rare in practice).
 * ====================================================================== */
#define HFAST   10
#define HFSZ    (1<<HFAST)
#define HCLEN   15
#define HALPHA  4376          /* 256+24+(1<<12) */

typedef struct { int16_t sym; uint8_t len; uint8_t pad; } HEntry;

typedef struct {
    HEntry fast[HFSZ];
    /* overflow: codes longer than HFAST bits */
    uint32_t ovf_code[256];  /* canonical code value */
    int16_t  ovf_sym [256];
    uint8_t  ovf_len [256];
    int      novf;
    int      maxlen;
    int      valid;
    int      default_sym;  /* single-symbol tree: return this without consuming bits */
} HTree;

/* Reverse `n` LSBs of `v` */
static uint32_t rev_bits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r<<1)|(v&1); v >>= 1; }
    return r;
}

static int htree_build(HTree *ht, const int *lens, int nsym) {
    ht->valid = 0; ht->maxlen = 0; ht->novf = 0; ht->default_sym = -1;
    wbp_memset(ht->fast, 0xFF, sizeof(ht->fast)); /* sym=-1 = empty */

    if (nsym <= 0 || nsym > HALPHA) return -1;

    /* Check for single-symbol special case first (VP8L: length-0 default sym).
     * Convention: lens[] has exactly one entry set to -1, indicating the
     * default symbol (no bits consumed).  read_code_lengths sets lens[sym]=-1
     * for the 1-symbol simple code. */
    for (int i = 0; i < nsym; i++) {
        if (lens[i] == -1) { ht->default_sym = i; ht->valid = 1; return 0; }
    }

    int cnt[HCLEN+2]; wbp_memset(cnt, 0, sizeof(cnt));
    int actual = 0;
    for (int i = 0; i < nsym; i++) {
        if (lens[i] < 0 || lens[i] > HCLEN) return -1;
        if (lens[i] > 0) cnt[lens[i]]++;
        if (lens[i] > ht->maxlen) ht->maxlen = lens[i];
        actual += (lens[i] > 0);
    }
    if (actual == 0) { ht->valid = 1; return 0; }

    /* Compute starting code per length (canonical Huffman) */
    int first[HCLEN+2]; wbp_memset(first, 0, sizeof(first));
    { int c = 0;
      for (int l = 1; l <= ht->maxlen; l++) {
          c = (c + cnt[l-1]) << 1;
          first[l] = c;
      }
    }
    int cur[HCLEN+2]; wbp_memcpy(cur, first, sizeof(first));

    for (int sym = 0; sym < nsym; sym++) {
        int l = lens[sym];
        if (l == 0) continue;
        int c = cur[l]++;
        if (l <= HFAST) {
            uint32_t rc = rev_bits((uint32_t)c, l);
            int step = 1 << l;
            for (int j = (int)rc; j < HFSZ; j += step) {
                ht->fast[j].sym = (int16_t)sym;
                ht->fast[j].len = (uint8_t)l;
            }
        } else {
            if (ht->novf >= 256) return -1;
            int k = ht->novf++;
            ht->ovf_code[k] = (uint32_t)c;
            ht->ovf_sym [k] = (int16_t)sym;
            ht->ovf_len [k] = (uint8_t)l;
        }
    }
    ht->valid = 1;
    return 0;
}

static int htree_sym(const HTree *ht, BR *br) {
    if (!ht->valid) { br->err = 1; return -1; }
    /* Single-symbol tree: return the default symbol without consuming bits */
    if (ht->default_sym >= 0) return ht->default_sym;
    if (ht->maxlen == 0) { br->err = 1; return -1; }
    br_fill(br);
    if (br->n >= HFAST || br->n >= ht->maxlen) {
        uint32_t idx = (uint32_t)(br->acc & (HFSZ-1));
        HEntry e = ht->fast[idx];
        if (e.sym >= 0 && e.len > 0 && e.len <= (uint8_t)br->n) {
            br->acc >>= e.len; br->n -= e.len;
            return e.sym;
        }
    } else {
        /* not enough bits yet; try partial match */
        if (br->n > 0) {
            uint32_t idx = (uint32_t)(br->acc & (HFSZ-1));
            HEntry e = ht->fast[idx];
            if (e.sym >= 0 && e.len > 0 && e.len <= (uint8_t)br->n) {
                br->acc >>= e.len; br->n -= e.len;
                return e.sym;
            }
        }
    }
    /* Slow path: bit-by-bit */
    br_fill(br);
    int code = 0;
    for (int l = 1; l <= ht->maxlen; l++) {
        br_fill(br);
        if (br->n < 1) { br->err = 1; return -1; }
        code = (code<<1) | (int)(br->acc&1);
        br->acc >>= 1; br->n--;
        /* fast table first */
        if (l <= HFAST) {
            uint32_t rc = rev_bits((uint32_t)code, l);
            HEntry e = ht->fast[rc];
            if (e.sym >= 0 && (int)e.len == l) return e.sym;
        }
        /* overflow */
        for (int k = 0; k < ht->novf; k++) {
            if (ht->ovf_len[k] == (uint8_t)l && ht->ovf_code[k] == (uint32_t)code)
                return ht->ovf_sym[k];
        }
    }
    br->err = 1; return -1;
}

/* =========================================================================
 * CLC (code-length code) tree — tiny fixed decoder for 19-symbol alphabet
 * ====================================================================== */
static const int kCLCOrder[19] = {
    17,18,0,1,2,3,4,5,16,6,7,8,9,10,11,12,13,14,15
};

/* Build a tiny HTree from the 19 CLC lengths using a stack-local array */
static int clc_build(HTree *out, int clc_lens[19]) {
    int lens[19];
    for (int i = 0; i < 19; i++) lens[i] = clc_lens[i];
    return htree_build(out, lens, 19);
}

/* =========================================================================
 * Read a Huffman code from the bitstream into lens[0..alpha_size)
 * ====================================================================== */
static int read_code_lengths(BR *br, int alpha_size, int *lens) {
    if (alpha_size <= 0 || alpha_size > HALPHA) return -1;
    wbp_memset(lens, 0, (long)alpha_size * sizeof(int));

    int simple = (int)br_read(br, 1);
    if (!br_ok(br)) return -1;

    if (simple) {
        /* 1 or 2 explicit symbols.
         * VP8L spec §6.2.1: simple Huffman tree format:
         *   num_symbols:     1 bit (0=1 sym, 1=2 syms)
         *   is_first_8bits:  1 bit (0=sym fits in 1 bit, 1=sym needs 8 bits)
         *   first_symbol:    1 or 8 bits depending on is_first_8bits
         *   [if 2 syms] second_symbol: always 8 bits
         */
        int nsym = (int)br_read(br, 1) + 1;
        if (!br_ok(br)) return -1;
        int is8 = (int)br_read(br, 1);
        if (!br_ok(br)) return -1;
        int s0 = (int)br_read(br, is8 ? 8 : 1);
        if (!br_ok(br)) return -1;
        if (s0 >= alpha_size) return -1;
        if (nsym == 1) {
            /* Single symbol: VP8L spec says "if only one symbol exists, the code
             * length is 0 and the decoder emits it without reading any bits."
             * We signal this with lens[s0]=-1 (special sentinel for htree_build). */
            lens[s0] = -1;
        } else {
            int s1 = (int)br_read(br, 8);
            if (!br_ok(br)) return -1;
            if (s1 >= alpha_size) return -1;
            if (s0 == s1) return -1;
            lens[s0] = 1; lens[s1] = 1;
        }
        return 0;
    }

    /* Complex code */
    int nclc = 4 + (int)br_read(br, 4);
    if (!br_ok(br) || nclc > 19) return -1;
    int clc_lens[19]; wbp_memset(clc_lens, 0, sizeof(clc_lens));
    for (int i = 0; i < nclc; i++) {
        clc_lens[kCLCOrder[i]] = (int)br_read(br, 3);
        if (!br_ok(br)) return -1;
    }

    HTree clc;
    if (clc_build(&clc, clc_lens) < 0) return -1;

    /* Read optional max_symbol.  Per spec/libwebp this is a COUNTDOWN on the
     * number of code-length CODES read (loop iterations), NOT on the symbol
     * index.  An RLE code in one iteration can fill many symbols. */
    int max_sym = alpha_size;
    if (br_read(br, 1)) {
        if (!br_ok(br)) return -1;
        int nb = 2 + 2*(int)br_read(br, 3);
        if (!br_ok(br)) return -1;
        max_sym = 2 + (int)br_read(br, nb);
        if (!br_ok(br)) return -1;
        if (max_sym > alpha_size) max_sym = alpha_size;
    } else {
        if (!br_ok(br)) return -1;
    }

    int prev = 8, i = 0;
    while (i < alpha_size) {
        if (max_sym-- == 0) break;   /* iteration budget exhausted */
        int s = htree_sym(&clc, br);
        if (!br_ok(br) || s < 0) return -1;
        if (s < 16) {
            lens[i++] = s;
            if (s) prev = s;
        } else if (s <= 18) {
            int r;
            if (s == 16) { r = 3 + (int)br_read(br,2); }
            else if (s == 17) { r = 3 + (int)br_read(br,3); }
            else /* 18 */ { r = 11 + (int)br_read(br,7); }
            if (!br_ok(br)) return -1;
            if (i + r > alpha_size) return -1;
            int fill = (s == 16) ? prev : 0;
            while (r-- > 0) lens[i++] = fill;
        } else {
            return -1;
        }
    }
    return 0;
}

/* =========================================================================
 * Huffman group (5 trees per group)
 * ====================================================================== */
#define HG_GREEN  0  /* green + length prefix (0..255+24) + cache */
#define HG_RED    1
#define HG_BLUE   2
#define HG_ALPHA  3
#define HG_DIST   4
#define HG_TREES  5

typedef struct { HTree t[HG_TREES]; } HGroup;

/* =========================================================================
 * Scratch allocator (bump-pointer)
 * ====================================================================== */
typedef struct { uint8_t *base; long cap; long used; } SA;

static void sa_init(SA *sa, uint8_t *buf, long cap) {
    sa->base = buf; sa->cap = cap; sa->used = 0;
}
static void *sa_alloc(SA *sa, long bytes) {
    long aligned = (sa->used + 7) & ~7L;
    if (aligned + bytes > sa->cap || bytes < 0) return 0;
    void *p = sa->base + aligned;
    sa->used = aligned + bytes;
    wbp_memset(p, 0, bytes);
    return p;
}

/* =========================================================================
 * VP8L length/distance prefix tables
 * ====================================================================== */
static int length_from_code(int code, BR *br) {
    /* code in [0..23].  VP8L uses the SAME prefix-coding scheme for lengths as
     * for distances (spec §4.2.2 / §5.4): for prefix<4 value=prefix+1, else
     * extra=(prefix-2)>>1, offset=(2+(prefix&1))<<extra, value=offset+1+bits.
     * (NOT the DEFLATE length table.) */
    static const int base[24] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073
    };
    static const int ext[24] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10
    };
    if (code < 0 || code >= 24) { br->err=1; return -1; }
    int ex = ext[code];
    int v = base[code];
    if (ex > 0) { v += (int)br_read(br, ex); if(!br_ok(br)) return -1; }
    return v;
}

static int dist_from_code(int code, BR *br) {
    /* code in [0..39] */
    static const int base[40] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577,
        32769,49153,65537,98305,131073,196609,262145,393217,524289,786433
    };
    static const int ext[40] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
        9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18
    };
    if (code < 0 || code >= 40) { br->err=1; return -1; }
    int ex = ext[code];
    int v = base[code];
    if (ex > 0) { v += (int)br_read(br, ex); if(!br_ok(br)) return -1; }
    return v;
}

/* Distance plane offsets (120 entries) */
static const int8_t kDP[120][2] = {
    {0,1},{1,0},{1,1},{-1,1},{0,2},{2,0},{1,2},{-1,2},{2,1},{-2,1},
    {2,2},{-2,2},{0,3},{3,0},{1,3},{-1,3},{3,1},{-3,1},{2,3},{-2,3},
    {3,2},{-3,2},{0,4},{4,0},{1,4},{-1,4},{4,1},{-4,1},{3,3},{-3,3},
    {2,4},{-2,4},{4,2},{-4,2},{0,5},{3,4},{-3,4},{4,3},{-4,3},{5,0},
    {1,5},{-1,5},{5,1},{-5,1},{2,5},{-2,5},{5,2},{-5,2},{4,4},{-4,4},
    {3,5},{-3,5},{5,3},{-5,3},{0,6},{6,0},{1,6},{-1,6},{6,1},{-6,1},
    {2,6},{-2,6},{6,2},{-6,2},{4,5},{-4,5},{5,4},{-5,4},{3,6},{-3,6},
    {6,3},{-6,3},{0,7},{7,0},{1,7},{-1,7},{5,5},{-5,5},{7,1},{-7,1},
    {4,6},{-4,6},{6,4},{-6,4},{2,7},{-2,7},{7,2},{-7,2},{3,7},{-3,7},
    {7,3},{-7,3},{5,6},{-5,6},{6,5},{-6,5},{8,0},{4,7},{-4,7},{7,4},
    {-7,4},{8,1},{1,8},{-1,8},{8,2},{2,8},{-2,8},{8,3},{3,8},{-3,8},
    {5,7},{-5,7},{7,5},{-7,5},{8,4},{4,8},{-4,8},{8,5},{5,8},{-5,8}
};

/* =========================================================================
 * Color cache hash
 * ====================================================================== */
static uint32_t cc_hash(uint32_t argb, int bits) {
    /* VP8L color-cache hash: the multiply is 32-bit (wraps mod 2^32), THEN the
     * top `bits` bits are taken.  Doing the multiply in 64-bit would leave more
     * than `bits` significant bits after the shift and index out of bounds. */
    return (uint32_t)((0x1e35a7bdU * argb) >> (32 - bits));
}

/* =========================================================================
 * Transform types
 * ====================================================================== */
#define TR_PREDICTOR      0
#define TR_COLOR          1
#define TR_SUBTRACT_GREEN 2
#define TR_COLOR_INDEXING 3
#define TR_MAX            4

typedef struct {
    int       type;
    int       bits;        /* block size log2 */
    uint32_t *data;        /* sub-image ARGB pixels */
    int       pal_size;    /* COLOR_INDEXING: palette size */
    int       width_bits;  /* COLOR_INDEXING: pack bits (0=no pack) */
} Transform;

/* =========================================================================
 * Forward declaration: decode a VP8L sub-image into `out` (uint32_t ARGB).
 * Used recursively for transform/entropy sub-images.
 * ====================================================================== */
static int decode_vp8l(BR *br, int w, int h, int is_sub,
                        uint32_t *out, long npix,
                        SA *sa);

/* =========================================================================
 * Inverse transforms
 * ====================================================================== */
static void inv_subtract_green(uint32_t *p, long n) {
    for (long i = 0; i < n; i++) {
        uint8_t g = (p[i] >>  8) & 0xFF;
        uint8_t r = (uint8_t)(((p[i] >> 16) & 0xFF) + g);
        uint8_t b = (uint8_t)(((p[i]      ) & 0xFF) + g);
        p[i] = (p[i] & 0xFF00FF00u) | ((uint32_t)r<<16) | b;
    }
}

static void inv_color(const uint32_t *cimg, int cb, int W, int H, uint32_t *pix) {
    int sw = (W + (1<<cb) - 1) >> cb;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t ct = cimg[(y>>cb)*sw + (x>>cb)];
            /* libwebp ColorCodeToMultipliers byte layout:
             *   green_to_red  = bits  0..7
             *   green_to_blue = bits  8..15
             *   red_to_blue   = bits 16..23 */
            int8_t g2r = (int8_t)((ct      )&0xFF);
            int8_t g2b = (int8_t)((ct >>  8)&0xFF);
            int8_t r2b = (int8_t)((ct >> 16)&0xFF);
            uint32_t px = pix[y*W+x];
            uint8_t r = (px>>16)&0xFF, g=(px>>8)&0xFF, b=px&0xFF, a=(px>>24)&0xFF;
            /* ColorTransformDelta(pred,c) = (pred * (int8)c) >> 5.  The colour
             * operand is interpreted as SIGNED int8, and the blue update uses
             * the NEWLY-corrected red (libwebp ColorSpaceInverseTransform). */
            int8_t gs = (int8_t)g;
            int nr = (int)r + (((int)g2r * (int)gs) >> 5);
            nr &= 0xFF;
            int nb = (int)b + (((int)g2b * (int)gs) >> 5)
                            + (((int)r2b * (int)(int8_t)nr) >> 5);
            nb &= 0xFF;
            pix[y*W+x] = ((uint32_t)a<<24)|((uint32_t)nr<<16)|
                         ((uint32_t)g<<8)|(uint32_t)nb;
        }
    }
}

/* Clamp integer to [0,255] */
static uint8_t clamp8(int v) { return v<0?0:(v>255?255:(uint8_t)v); }

static uint32_t predictor(int mode, uint32_t TL, uint32_t T, uint32_t TR, uint32_t L) {
#define CH(p,sh) (((p)>>(sh))&0xFF)
#define AVG2(a,b) (((unsigned)(a)+(unsigned)(b))>>1)
#define AVG3(a,b,c) ((((unsigned)(a)+(unsigned)(b))>>1)+(unsigned)(c))>>1
    switch (mode & 0xF) {
    case 0: return 0xFF000000u;
    case 1: return L;
    case 2: return T;
    case 3: return TR;
    case 4: return TL;
    case 5: /* avg(avg(L,TR),T) */
        return ((uint32_t)((AVG2(CH(L,24),CH(TR,24))+CH(T,24))>>1)<<24)|
               ((uint32_t)((AVG2(CH(L,16),CH(TR,16))+CH(T,16))>>1)<<16)|
               ((uint32_t)((AVG2(CH(L, 8),CH(TR, 8))+CH(T, 8))>>1)<< 8)|
               (uint32_t)((AVG2(CH(L, 0),CH(TR, 0))+CH(T, 0))>>1);
    case 6:  /* avg(L,TL) rounded */
        return ((uint32_t)AVG2(CH(L,24),CH(TL,24))<<24)|((uint32_t)AVG2(CH(L,16),CH(TL,16))<<16)|
               ((uint32_t)AVG2(CH(L, 8),CH(TL, 8))<< 8)|(uint32_t)AVG2(CH(L, 0),CH(TL, 0));
    case 7:  /* avg(L,T) */
        return ((uint32_t)AVG2(CH(L,24),CH(T,24))<<24)|((uint32_t)AVG2(CH(L,16),CH(T,16))<<16)|
               ((uint32_t)AVG2(CH(L, 8),CH(T, 8))<< 8)|(uint32_t)AVG2(CH(L, 0),CH(T, 0));
    case 8:  /* avg(TL,T) */
        return ((uint32_t)AVG2(CH(TL,24),CH(T,24))<<24)|((uint32_t)AVG2(CH(TL,16),CH(T,16))<<16)|
               ((uint32_t)AVG2(CH(TL, 8),CH(T, 8))<< 8)|(uint32_t)AVG2(CH(TL, 0),CH(T, 0));
    case 9:  /* avg(T,TR) */
        return ((uint32_t)AVG2(CH(T,24),CH(TR,24))<<24)|((uint32_t)AVG2(CH(T,16),CH(TR,16))<<16)|
               ((uint32_t)AVG2(CH(T, 8),CH(TR, 8))<< 8)|(uint32_t)AVG2(CH(T, 0),CH(TR, 0));
    case 10: /* avg(L,T) -- same as 7 in spec */
        return ((uint32_t)AVG2(CH(L,24),CH(T,24))<<24)|((uint32_t)AVG2(CH(L,16),CH(T,16))<<16)|
               ((uint32_t)AVG2(CH(L, 8),CH(T, 8))<< 8)|(uint32_t)AVG2(CH(L, 0),CH(T, 0));
    case 11: { /* Select(L,T,TL) */
        int pL = 0, pT = 0;
        for (int sh = 0; sh <= 24; sh += 8) {
            int d = CH(TL,sh);
            int dl = d - (int)CH(L,sh); if(dl<0)dl=-dl;
            int dt = d - (int)CH(T,sh); if(dt<0)dt=-dt;
            pL += dl; pT += dt;
        }
        return (pL <= pT) ? L : T;
    }
    case 12: /* ClampAddSubtractFull */
        return ((uint32_t)clamp8((int)CH(L,24)+(int)CH(T,24)-(int)CH(TL,24))<<24)|
               ((uint32_t)clamp8((int)CH(L,16)+(int)CH(T,16)-(int)CH(TL,16))<<16)|
               ((uint32_t)clamp8((int)CH(L, 8)+(int)CH(T, 8)-(int)CH(TL, 8))<< 8)|
               (uint32_t)clamp8((int)CH(L, 0)+(int)CH(T, 0)-(int)CH(TL, 0));
    case 13: /* ClampAddSubtractHalf */
        return ((uint32_t)clamp8((int)CH(L,24)+((int)CH(T,24)-(int)CH(TL,24))/2)<<24)|
               ((uint32_t)clamp8((int)CH(L,16)+((int)CH(T,16)-(int)CH(TL,16))/2)<<16)|
               ((uint32_t)clamp8((int)CH(L, 8)+((int)CH(T, 8)-(int)CH(TL, 8))/2)<< 8)|
               (uint32_t)clamp8((int)CH(L, 0)+((int)CH(T, 0)-(int)CH(TL, 0))/2);
    default: return L;
    }
#undef CH
#undef AVG2
#undef AVG3
}

static uint32_t argb_add(uint32_t a, uint32_t b) {
    return (uint32_t)(((a>>24)+(b>>24)&0xFF)<<24)|
           (uint32_t)(((((a>>16)&0xFF)+((b>>16)&0xFF))&0xFF)<<16)|
           (uint32_t)(((((a>>8)&0xFF)+((b>>8)&0xFF))&0xFF)<<8)|
           (uint32_t)(((a&0xFF)+(b&0xFF))&0xFF);
}

static void inv_predictor(const uint32_t *pimg, int cb, int W, int H, uint32_t *px) {
    int sw = (W + (1<<cb) - 1) >> cb;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            long idx = (long)y*W + x;
            uint32_t pred;
            if (x == 0 && y == 0) {
                pred = 0xFF000000u;
            } else if (y == 0) {
                pred = px[idx-1]; /* left */
            } else if (x == 0) {
                pred = px[idx-W]; /* top */
            } else {
                uint32_t blk = pimg[(y>>cb)*sw + (x>>cb)];
                int mode = (int)((blk>>8)&0xFF); /* green byte = predictor type */
                uint32_t L  = px[idx-1];
                uint32_t T  = px[idx-W];
                uint32_t TR = (x+1<W) ? px[idx-W+1] : px[idx-W]; /* right edge: repeat T */
                uint32_t TL = px[idx-W-1];
                pred = predictor(mode, TL, T, TR, L);
            }
            px[idx] = argb_add(px[idx], pred);
        }
    }
}

static int inv_color_indexing(const Transform *tr, int orig_W, int H,
                               uint32_t *pix, long total_pix) {
    int wb = tr->width_bits;
    int ps = tr->pal_size;
    if (ps <= 0 || ps > 256) return -1;

    if (wb == 0) {
        /* 1 index/pixel */
        for (long i = 0; i < total_pix; i++) {
            int idx = (int)((pix[i]>>8)&0xFF) & (ps-1);
            pix[i] = tr->data[idx];
        }
    } else {
        /* Packed: `pack` indices per byte (from the green byte) */
        int pack        = 1 << wb;   /* pixels per packed byte */
        int bits_idx    = 8 >> wb;   /* bits per index */
        int mask        = (1 << bits_idx) - 1;  /* per-index bit mask */
        long packed_W   = ((long)orig_W + pack - 1) / pack;
        /* Unpack right-to-left to avoid overwriting source */
        for (int y = H-1; y >= 0; y--) {
            long src_row = (long)y * packed_W;
            long dst_row = (long)y * orig_W;
            long x_out   = orig_W - 1;
            for (long px2 = packed_W-1; px2 >= 0; px2--) {
                uint8_t byte = (uint8_t)((pix[src_row+px2]>>8)&0xFF);
                int start = (int)(px2 * pack);
                int end   = start + pack;
                if (end > orig_W) end = orig_W;
                int count = end - start;
                for (int k = count-1; k >= 0; k--, x_out--) {
                    int idx = (byte >> (k * bits_idx)) & mask;
                    if (idx >= ps) idx = 0;
                    pix[dst_row + x_out] = tr->data[idx];
                }
            }
        }
    }
    return 0;
}

/* =========================================================================
 * Core VP8L image decoder
 *
 * Bitstream order:
 *   1. Transforms (while read_bit(1): 2-bit type + data)
 *   2. Color cache (1 bit present; 4-bit code_bits if present)
 *   3. Meta-Huffman (1 bit use_meta; if set: huffman_bits + entropy sub-image)
 *   4. Huffman code group(s)
 *   5. Pixel data
 *   6. Apply inverse transforms in reverse order
 *
 * is_sub=1: sub-image (for transforms / palette / entropy image);
 *           skip the transforms and meta-huffman (they come "free" as simple
 *           literal images under a fresh Huffman setup).
 *           Actually per spec sub-images ARE full VP8L images including their
 *           own color-cache and transforms — but in practice libwebp doesn't
 *           nest transforms in sub-images, so we support them but they rarely
 *           occur.
 * ====================================================================== */
#define MAX_HGROUPS 512

static int decode_vp8l(BR *br, int W, int H, int is_sub,
                        uint32_t *out, long npix,
                        SA *sa) {
    if (W<=0||H<=0||W>2048||H>2048||(long)W*H>npix) return -1;

    long sa_save = sa->used;  /* snapshot so sub-calls don't permanently consume */
    (void)sa_save;            /* we do NOT restore — sub-images are permanent */

    /* ---- 1. Transforms ---- */
    /* Sub-images do NOT read transforms (libwebp: only is_level0=1 reads transforms). */
    Transform trs[TR_MAX];
    int ntr = 0, used_tr = 0;

    while (!is_sub && br_read(br, 1)) {
        if (!br_ok(br)) return -1;
        if (ntr >= TR_MAX) return -1;
        int tt = (int)br_read(br, 2);
        if (!br_ok(br)) return -1;
        if (used_tr & (1<<tt)) return -1; /* duplicate */
        used_tr |= (1<<tt);

        Transform *tr = &trs[ntr++];
        tr->type = tt; tr->bits = 0; tr->data = 0;
        tr->pal_size = 0; tr->width_bits = 0;

        if (tt == TR_PREDICTOR || tt == TR_COLOR) {
            tr->bits = (int)br_read(br, 3) + 2;
            if (!br_ok(br)) return -1;
            int sw = (W+(1<<tr->bits)-1)>>tr->bits;
            int sh = (H+(1<<tr->bits)-1)>>tr->bits;
            long sn = (long)sw*sh;
            if (sn<=0||sn>(1<<20)) return -1;
            tr->data = (uint32_t *)sa_alloc(sa, sn*4);
            if (!tr->data) return -1;
            if (decode_vp8l(br, sw, sh, 1, tr->data, sn, sa) < 0) return -1;
        } else if (tt == TR_SUBTRACT_GREEN) {
            /* no data */
        } else if (tt == TR_COLOR_INDEXING) {
            tr->pal_size = (int)br_read(br, 8) + 1;
            if (!br_ok(br)||tr->pal_size>256) return -1;
            tr->data = (uint32_t *)sa_alloc(sa, (long)tr->pal_size*4);
            if (!tr->data) return -1;
            if (decode_vp8l(br, tr->pal_size, 1, 1, tr->data,
                            tr->pal_size, sa) < 0) return -1;
            /* Delta-decode palette */
            for (int k=1; k<tr->pal_size; k++) {
                uint32_t prev=tr->data[k-1], cur=tr->data[k];
                tr->data[k] =
                    (uint32_t)(((cur>>24)&0xFF)+((prev>>24)&0xFF))<<24|
                    (uint32_t)((((cur>>16)&0xFF)+((prev>>16)&0xFF))&0xFF)<<16|
                    (uint32_t)((((cur>>8 )&0xFF)+((prev>>8 )&0xFF))&0xFF)<<8 |
                    (uint32_t)(((cur&0xFF)+( prev&0xFF))&0xFF);
            }
            /* Width-bits for bundling */
            tr->width_bits = (tr->pal_size<=2)?3:(tr->pal_size<=4)?2:
                              (tr->pal_size<=16)?1:0;
        } else return -1;
    }
    if (!br_ok(br)) return -1;

    /* Actual decode width after bundling */
    int dec_W = W;
    for (int t = 0; t < ntr; t++) {
        if (trs[t].type==TR_COLOR_INDEXING && trs[t].width_bits>0) {
            int pk = 1<<trs[t].width_bits;
            dec_W = (W + pk - 1) / pk;
            break;
        }
    }
    long dec_npix = (long)dec_W * H;

    /* ---- 2. Color cache ---- */
    int use_cc = (int)br_read(br, 1);
    if (!br_ok(br)) return -1;
    int cc_bits = 0, cc_size = 0;
    uint32_t *ccache = 0;
    if (use_cc) {
        cc_bits = (int)br_read(br, 4);
        if (!br_ok(br)) return -1;
        if (cc_bits < 1 || cc_bits > 12) return -1;
        cc_size = 1 << cc_bits;
        ccache  = (uint32_t *)sa_alloc(sa, (long)cc_size * 4);
        if (!ccache) return -1;
    }

    /* ---- 3. Meta-Huffman ---- */
    int use_meta = 0, hb = 0, meta_W = 0, n_groups = 1;
    uint32_t *mimg = 0;

    /* Sub-images never use meta-Huffman (ReadHuffmanCodes with allow_recursion=0). */
    if (!is_sub)
        use_meta = (int)br_read(br, 1);
    if (!br_ok(br)) return -1;

    if (use_meta) {
        hb = (int)br_read(br, 3) + 2;
        if (!br_ok(br)) return -1;
        meta_W = (dec_W+(1<<hb)-1)>>hb;
        int meta_H = (H+(1<<hb)-1)>>hb;
        long meta_n = (long)meta_W*meta_H;
        if (meta_n<=0||meta_n>(1<<20)) return -1;
        mimg = (uint32_t *)sa_alloc(sa, meta_n*4);
        if (!mimg) return -1;
        if (decode_vp8l(br, meta_W, meta_H, 1, mimg, meta_n, sa) < 0) return -1;
        /* Find max group index */
        for (long k=0; k<meta_n; k++) {
            int g = (int)((mimg[k]>>8)&0xFFFF);
            if (g+1>n_groups) n_groups=g+1;
        }
        if (n_groups > MAX_HGROUPS) return -1;
    }

    /* ---- 4. Huffman code groups ---- */
    int alpha_g = 256 + 24 + cc_size;

    HGroup *groups = (HGroup *)sa_alloc(sa, (long)n_groups * (long)sizeof(HGroup));
    if (!groups) return -1;

    /* Temporary lens array on stack — max alpha size */
    int lens[HALPHA]; /* stack allocation: 4376*4 = ~17KB, acceptable */

    for (int g = 0; g < n_groups; g++) {
        HGroup *grp = &groups[g];
#define DBGBIT(tag)
        DBGBIT("pre-GREEN");
        if (read_code_lengths(br, alpha_g, lens) < 0) return -1;
        if (htree_build(&grp->t[HG_GREEN], lens, alpha_g) < 0) return -1;
        DBGBIT("pre-RED");
        if (read_code_lengths(br, 256, lens) < 0) return -1;
        if (htree_build(&grp->t[HG_RED], lens, 256) < 0) return -1;
        DBGBIT("pre-BLUE");
        if (read_code_lengths(br, 256, lens) < 0) return -1;
        if (htree_build(&grp->t[HG_BLUE], lens, 256) < 0) return -1;
        DBGBIT("pre-ALPHA");
        if (read_code_lengths(br, 256, lens) < 0) return -1;
        if (htree_build(&grp->t[HG_ALPHA], lens, 256) < 0) return -1;
        DBGBIT("pre-DIST");
        if (read_code_lengths(br, 40, lens) < 0) return -1;
        if (htree_build(&grp->t[HG_DIST], lens, 40) < 0) return -1;
    }

    /* ---- 5. Pixel decode loop ---- */
    if (dec_npix > (1L<<20)) return -1;
    if (dec_npix > npix) return -1;

    long pos = 0;
    while (pos < dec_npix) {
        /* Determine Huffman group */
        int gi = 0;
        if (use_meta && mimg) {
            int px = (int)(pos % dec_W), py = (int)(pos / dec_W);
            int bx = px>>hb, by = py>>hb;
            if (bx >= meta_W) bx = meta_W-1;
            gi = (int)((mimg[by*meta_W + bx]>>8)&0xFFFF);
            if (gi >= n_groups) gi = 0;
        }
        HGroup *grp = &groups[gi];

        int S = htree_sym(&grp->t[HG_GREEN], br);
        if (!br_ok(br)||S<0) return -1;

        if (S < 256) {
            /* literal */
            int R = htree_sym(&grp->t[HG_RED],   br); if(!br_ok(br)||R<0) return -1;
            int B = htree_sym(&grp->t[HG_BLUE],  br); if(!br_ok(br)||B<0) return -1;
            int A = htree_sym(&grp->t[HG_ALPHA], br); if(!br_ok(br)||A<0) return -1;
            uint32_t argb = ((uint32_t)A<<24)|((uint32_t)R<<16)|((uint32_t)S<<8)|(uint32_t)B;
            out[pos] = argb;
            if (cc_size > 0) { uint32_t h=cc_hash(argb,cc_bits); ccache[h]=argb; }
            pos++;
        } else if (S < 256+24) {
            /* LZ77 back-reference */
            int len_code = S - 256;
            int copy_len = length_from_code(len_code, br);
            if (!br_ok(br)||copy_len<1) return -1;

            int dist_sym = htree_sym(&grp->t[HG_DIST], br);
            if (!br_ok(br)||dist_sym<0||dist_sym>=40) return -1;
            int dist_raw = dist_from_code(dist_sym, br);
            if (!br_ok(br)||dist_raw<1) return -1;

            long dist;
            if (dist_raw <= 120) {
                int xi = kDP[dist_raw-1][0], yi = kDP[dist_raw-1][1];
                dist = (long)xi + (long)yi * dec_W;
                if (dist < 1) dist = 1;
            } else {
                dist = dist_raw - 120;
            }
            if (dist < 1 || dist > pos) return -1;
            if (pos + copy_len > dec_npix) return -1;

            for (int k = 0; k < copy_len; k++) {
                uint32_t argb = out[pos - dist];
                out[pos] = argb;
                if (cc_size > 0) { uint32_t h=cc_hash(argb,cc_bits); ccache[h]=argb; }
                pos++;
            }
        } else {
            /* Color cache */
            int cidx = S - (256+24);
            if (!use_cc || cidx >= cc_size) return -1;
            uint32_t argb = ccache[cidx];
            out[pos] = argb;
            if (cc_size > 0) { uint32_t h=cc_hash(argb,cc_bits); ccache[h]=argb; }
            pos++;
        }
    }


    /* ---- 6. Inverse transforms (reverse order) ---- */
    for (int t = ntr-1; t >= 0; t--) {
        Transform *tr = &trs[t];
        switch (tr->type) {
        case TR_SUBTRACT_GREEN:
            inv_subtract_green(out, dec_npix);
            break;
        case TR_COLOR:
            inv_color(tr->data, tr->bits, dec_W, H, out);
            break;
        case TR_PREDICTOR:
            inv_predictor(tr->data, tr->bits, dec_W, H, out);
            break;
        case TR_COLOR_INDEXING:
            if (inv_color_indexing(tr, W, H, out, dec_npix) < 0) return -1;
            break;
        }
    }

    return 0;
}

/* =========================================================================
 * Public API
 * ====================================================================== */
int webp_probe(const uint8_t *data, int len, int *w, int *h, long *scratch_need) {
    if (!data||!w||!h||!scratch_need) return -1;
    int pl = 0;
    int off = find_vp8l(data, len, &pl);
    if (off<0||pl<5) return -1;
    const uint8_t *p = data + off;
    if (p[0] != 0x2F) return -1;

    BR br; br_init(&br, p+1, pl-1);
    int iw = (int)br_read(&br, 14)+1;
    int ih = (int)br_read(&br, 14)+1;
    br_read(&br, 1); /* alpha_used */
    int ver = (int)br_read(&br, 3);
    if (!br_ok(&br)||ver!=0) return -1;
    if (iw<=0||ih<=0||iw>2048||ih>2048) return -1;
    if ((long)iw*ih>(1<<20)) return -1;

    *w = iw; *h = ih;
    /* Scratch estimate: we need the ARGB pixel buffer + sub-image buffers +
     * Huffman groups.  sizeof(HGroup) with HTree internals:
     *   HEntry[1024]*4 + 256*(4+2+1)*3 + 5 * HG_TREES overhead ≈ 12KB/group
     * 512 groups × 5 trees × 12KB = 30MB in the worst case.
     * In practice (typical web images): 1-4 groups, <1MB.
     * We request a generous fixed buffer so the caller can pre-allocate. */
    long pix = (long)iw*ih*4;
    *scratch_need = pix * 8 + (16L<<20); /* 16 MB fixed overhead */
    if (*scratch_need < (24L<<20)) *scratch_need = 24L<<20;
    return 0;
}

int webp_decode(const uint8_t *data, int len,
                uint8_t *rgba, int rgba_cap,
                uint8_t *scratch, int scr_cap,
                int *ow, int *oh) {
    if (!data||!rgba||!scratch||!ow||!oh) return -1;
    if (len<20||scr_cap<0) return -1;

    int pl = 0;
    int off = find_vp8l(data, len, &pl);
    if (off<0||pl<5) return -1;
    const uint8_t *p = data + off;
    if (p[0] != 0x2F) return -1;

    BR br; br_init(&br, p+1, pl-1);
    int iw = (int)br_read(&br, 14)+1;
    int ih = (int)br_read(&br, 14)+1;
    br_read(&br, 1);
    int ver = (int)br_read(&br, 3);
    if (!br_ok(&br)||ver!=0) return -1;
    if (iw<=0||ih<=0||iw>2048||ih>2048) return -1;
    if ((long)iw*ih>(1<<20)) return -1;

    long npix = (long)iw*ih;
    long rgba_need = npix*4;
    if (rgba_cap < rgba_need) return -1;

    /* Carve the ARGB pixel buffer out of scratch */
    long argb_bytes = npix * 4;
    if ((long)scr_cap < argb_bytes + (4L<<20)) return -1; /* need ≥ 4MB overhead */
    uint32_t *argb_buf = (uint32_t *)(void *)scratch;
    uint8_t  *rem      = scratch + argb_bytes;
    long      rem_cap  = (long)scr_cap - argb_bytes;

    SA sa; sa_init(&sa, rem, rem_cap);

    if (decode_vp8l(&br, iw, ih, 0, argb_buf, npix, &sa) < 0) return -1;

    /* ARGB → RGBA */
    for (long i = 0; i < npix; i++) {
        uint32_t px = argb_buf[i];
        rgba[i*4+0] = (px>>16)&0xFF; /* R */
        rgba[i*4+1] = (px>> 8)&0xFF; /* G */
        rgba[i*4+2] = (px    )&0xFF; /* B */
        rgba[i*4+3] = (px>>24)&0xFF; /* A */
    }
    *ow = iw; *oh = ih;
    return 0;
}
