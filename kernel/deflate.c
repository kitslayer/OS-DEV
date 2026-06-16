/*
 * deflate.c — RFC-1951 DEFLATE compressor + RFC-1952 gzip wrapper.
 *
 * The encoding counterpart to inflate.c. Produces a complete gzip stream from
 * arbitrary input using a single fixed-Huffman DEFLATE block (BTYPE=01) with
 * LZ77 back-references found via a 3-byte hash chain over a 32 KB window.
 * Freestanding: depends only on <stdint.h>/<stddef.h> (no libc, no malloc, no
 * floating point). Every byte written to `out` is bounded against `outcap`, so
 * it is safe to run in the kernel with no stack guard page.
 *
 * Bit/byte ordering (must mirror inflate.c's reader exactly):
 *   - Bits are packed into bytes LSB-first (the reader's getbit() consumes the
 *     low bit of each byte first).
 *   - Huffman codes are emitted MSB-first per RFC 1951 §3.1.1: the reader's
 *     decode() shifts its accumulator left and ORs each new bit into the LSB,
 *     so the first bit it reads is the most-significant bit of the code.
 *   - Extra bits and the stored-block LEN/NLEN are little-endian / LSB-first.
 */
#include "inflate.h"
#include <stddef.h>

/* ------------------------------------------------------------------ CRC-32 */
/* Standard reflected CRC-32 (polynomial 0xEDB88320, as used by gzip/zlib/PNG).
 * Built once into a 256-entry table on first use. */
static uint32_t crc_tab[256];
static int      crc_ready = 0;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_buf(const uint8_t *p, int len) {
    if (!crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++)
        c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ----------------------------------------------------------- bit writer */
/* Accumulates bits LSB-first into a byte buffer, every store bounded by cap.
 * Once `oom` is set (an attempted write would exceed cap) all further writes
 * are silently dropped and the caller turns it into a -1 return. */
typedef struct {
    uint8_t *out;
    int      cap;
    int      pos;
    uint32_t acc;   /* pending bits, LSB-first */
    int      nbits; /* number of valid bits in acc (0..7 after each flush) */
    int      oom;   /* set if a write would exceed cap */
} bitw;

static void put_byte(bitw *w, uint8_t b) {
    if (w->pos >= w->cap) { w->oom = 1; return; }
    w->out[w->pos++] = b;
}

/* Emit `n` bits (0..24) of `val`, low bit first into the stream. */
static void put_bits(bitw *w, uint32_t val, int n) {
    w->acc |= (val & ((n < 32) ? ((1u << n) - 1u) : 0xFFFFFFFFu)) << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        put_byte(w, (uint8_t)(w->acc & 0xFF));
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

/* Emit a Huffman code of `n` bits MSB-first: reverse the code so its MSB lands
 * in the first stream bit, then push it LSB-first through put_bits(). */
static void put_huff(bitw *w, uint32_t code, int n) {
    uint32_t rev = 0;
    for (int i = 0; i < n; i++) {
        rev = (rev << 1) | (code & 1);
        code >>= 1;
    }
    put_bits(w, rev, n);
}

/* Pad the final partial byte with zero bits and flush it. */
static void flush_bits(bitw *w) {
    if (w->nbits > 0) {
        put_byte(w, (uint8_t)(w->acc & 0xFF));
        w->acc = 0;
        w->nbits = 0;
    }
}

/* ------------------------------------------------- fixed-Huffman code tables */
/* Canonical code lengths (RFC 1951 §3.2.6). We build the canonical code VALUES
 * algorithmically (the same procedure inflate.c's build() inverts) so they are
 * guaranteed consistent with the decoder rather than hand-transcribed. */
static uint16_t lit_code[288];   /* literal/length symbols 0..287 */
static uint8_t  lit_len [288];
static uint16_t dist_code[30];   /* distance symbols 0..29 */
static uint8_t  dist_lenb[30];
static int      fixed_ready = 0;

/* Assign canonical Huffman codes from a length array, per RFC 1951 §3.2.2:
 *   bl_count -> next_code (smallest code of each length) -> per-symbol code. */
static void canon(const uint8_t *lengths, int n, uint16_t *codes) {
    uint16_t bl_count[16] = {0};
    for (int i = 0; i < n; i++) bl_count[lengths[i]]++;
    bl_count[0] = 0;
    uint16_t next_code[16] = {0};
    uint16_t code = 0;
    for (int bits = 1; bits <= 15; bits++) {
        code = (uint16_t)((code + bl_count[bits - 1]) << 1);
        next_code[bits] = code;
    }
    for (int i = 0; i < n; i++) {
        int len = lengths[i];
        if (len) codes[i] = next_code[len]++;
        else     codes[i] = 0;
    }
}

static void fixed_init(void) {
    for (int i = 0;   i < 144; i++) lit_len[i] = 8;
    for (int i = 144; i < 256; i++) lit_len[i] = 9;
    for (int i = 256; i < 280; i++) lit_len[i] = 7;
    for (int i = 280; i < 288; i++) lit_len[i] = 8;
    canon(lit_len, 288, lit_code);
    for (int i = 0; i < 30; i++) dist_lenb[i] = 5;
    canon(dist_lenb, 30, dist_code);
    fixed_ready = 1;
}

static void emit_lit(bitw *w, int sym) {
    put_huff(w, lit_code[sym], lit_len[sym]);
}

/* --------------------------------------------- length/distance code mappings */
/* RFC 1951 §3.2.5 tables (kept in sync with inflate.c's lbase/lext/dbase/dext).
 * length_code()/dist_code() find the symbol whose base..base+extra range covers
 * the value, plus the number of extra bits and the extra value to append. */
static const short Lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short Lext [29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short Dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short Dext [30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

/* Map a match length (3..258) to its length symbol (257..285) + extra bits. */
static void encode_length(bitw *w, int len) {
    int sym = 28;                              /* default: 258 -> last symbol */
    for (int i = 0; i < 29; i++) {
        int hi = (i < 28) ? (Lbase[i + 1] - 1) : 258;
        if (len >= Lbase[i] && len <= hi) { sym = i; break; }
    }
    emit_lit(w, 257 + sym);
    if (Lext[sym]) put_bits(w, (uint32_t)(len - Lbase[sym]), Lext[sym]);
}

/* Map a match distance (1..32768) to its distance symbol (0..29) + extra bits. */
static void encode_distance(bitw *w, int dist) {
    int sym = 29;
    for (int i = 0; i < 30; i++) {
        int hi = (i < 29) ? (Dbase[i + 1] - 1) : 32768;
        if (dist >= Dbase[i] && dist <= hi) { sym = i; break; }
    }
    put_huff(w, dist_code[sym], dist_lenb[sym]);
    if (Dext[sym]) put_bits(w, (uint32_t)(dist - Dbase[sym]), Dext[sym]);
}

/* --------------------------------------------------------- LZ77 match finder */
#define WSIZE       32768            /* 32 KB sliding window (max distance) */
#define MIN_MATCH   3
#define MAX_MATCH   258
#define HASH_BITS   15
#define HASH_SIZE   (1 << HASH_BITS)
#define HASH_MASK   (HASH_SIZE - 1)
#define NIL         (-1)
#define MAX_CHAIN   128              /* bounded chain depth (anti-blowup) */

/* Static scratch (BSS, no malloc): hash heads + the prev[] chain. prev[] is
 * indexed by position & (WSIZE-1) and links each position to the previous one
 * with the same hash, forming per-bucket chains we walk back over. */
static int head[HASH_SIZE];
static int prev[WSIZE];

static int hash3(const uint8_t *p) {
    /* 3-byte rolling hash; the exact mix only affects ratio, not correctness. */
    uint32_t h = ((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2];
    return (int)((h * 2654435761u >> (32 - HASH_BITS)) & HASH_MASK);
}

/* ------------------------------------------------------------- raw DEFLATE */
/* Compress src[0..len) as a single fixed-Huffman block with BFINAL=1, writing
 * to the bit writer. Returns 0, or -1 if it ran out of output space. */
static int deflate_fixed(bitw *w, const uint8_t *src, int len) {
    if (!fixed_ready) fixed_init();
    for (int i = 0; i < HASH_SIZE; i++) head[i] = NIL;

    /* Block header: BFINAL=1 (1 bit), BTYPE=01 fixed (2 bits, value 1). */
    put_bits(w, 1, 1);
    put_bits(w, 1, 2);

    int i = 0;
    while (i < len) {
        int best_len = 0, best_dist = 0;

        /* Only look for matches when at least MIN_MATCH bytes remain. */
        if (i + MIN_MATCH <= len) {
            int h = hash3(src + i);
            int cur = head[h];
            int chain = MAX_CHAIN;
            int limit = len - i;
            if (limit > MAX_MATCH) limit = MAX_MATCH;

            while (cur != NIL && chain--) {
                int dist = i - cur;
                if (dist <= 0 || dist > WSIZE) break;   /* outside the window */
                /* Compare; require beating the current best to take it. */
                if (src[cur + best_len] == src[i + best_len]) {
                    int l = 0;
                    while (l < limit && src[cur + l] == src[i + l]) l++;
                    if (l > best_len) {
                        best_len  = l;
                        best_dist = dist;
                        if (l >= limit) break;          /* can't do better */
                    }
                }
                cur = prev[cur & (WSIZE - 1)];
            }
        }

        if (best_len >= MIN_MATCH) {
            encode_length(w, best_len);
            encode_distance(w, best_dist);
            /* Insert every position the match covers so future matches can
             * reference inside it. Bounded by remaining input. */
            int end = i + best_len;
            for (; i < end; i++) {
                if (i + MIN_MATCH <= len) {
                    int h = hash3(src + i);
                    prev[i & (WSIZE - 1)] = head[h];
                    head[h] = i;
                }
            }
        } else {
            emit_lit(w, src[i]);
            if (i + MIN_MATCH <= len) {
                int h = hash3(src + i);
                prev[i & (WSIZE - 1)] = head[h];
                head[h] = i;
            }
            i++;
        }
        if (w->oom) return -1;
    }

    emit_lit(w, 256);          /* end-of-block symbol */
    flush_bits(w);
    return w->oom ? -1 : 0;
}

/* Public: compress raw DEFLATE (no wrapper) into out[0..outcap). Returns bytes
 * written, or -1 on overflow. Provided as the split-out helper. */
int raw_deflate(const uint8_t *src, int len, uint8_t *out, int outcap) {
    bitw w = { out, outcap, 0, 0, 0, 0 };
    if (deflate_fixed(&w, src, len) < 0) return -1;
    return w.pos;
}

/* ------------------------------------------------------------------- gzip */
/* Wrap raw DEFLATE in a complete RFC-1952 gzip stream:
 *   [0]   0x1f 0x8b            magic
 *   [2]   0x08                 method = DEFLATE
 *   [3]   0x00                 flags (no optional fields)
 *   [4]   0x00000000           mtime (none)
 *   [8]   0x00                 XFL
 *   [9]   0xff                 OS = unknown
 *   ...   DEFLATE body
 *   tail  CRC32(input) LE, then ISIZE = len mod 2^32 LE
 * Returns total bytes written to out, or -1 if it would exceed outcap. */
int gz_deflate(const uint8_t *src, int len, uint8_t *out, int outcap) {
    if (len < 0) return -1;

    static const uint8_t hdr[10] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff
    };
    if (10 > outcap) return -1;
    for (int k = 0; k < 10; k++) out[k] = hdr[k];

    int body = raw_deflate(src, len, out + 10, outcap - 10);
    if (body < 0) return -1;

    int p = 10 + body;
    if (p + 8 > outcap) return -1;

    uint32_t crc = crc32_buf(src, len);
    uint32_t isize = (uint32_t)len;            /* len mod 2^32 (len fits int) */
    out[p++] = (uint8_t)(crc & 0xFF);
    out[p++] = (uint8_t)((crc >> 8) & 0xFF);
    out[p++] = (uint8_t)((crc >> 16) & 0xFF);
    out[p++] = (uint8_t)((crc >> 24) & 0xFF);
    out[p++] = (uint8_t)(isize & 0xFF);
    out[p++] = (uint8_t)((isize >> 8) & 0xFF);
    out[p++] = (uint8_t)((isize >> 16) & 0xFF);
    out[p++] = (uint8_t)((isize >> 24) & 0xFF);
    return p;
}
