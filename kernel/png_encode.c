/*
 * png_encode.c — a minimal from-scratch PNG encoder (RFC 2083), the encoding
 * counterpart to png.c's decoder. Emits an 8-bit, non-interlaced, truecolour
 * (colour type 2, RGB) PNG: the signature, an IHDR chunk, a single IDAT chunk
 * carrying a zlib stream (RFC 1950) whose DEFLATE body comes from raw_deflate()
 * (kernel/deflate.c), and an IEND chunk.
 *
 * Scanline filtering uses filter type 0 (None) for every row: the pre-
 * compression buffer is, per scanline, one 0x00 filter byte followed by that
 * row's w*3 RGB bytes. That filtered buffer is built in caller-provided
 * `scratch` ((1 + w*3)*h bytes) and is what gets compressed and Adler-32'd.
 *
 * Freestanding: depends only on <stdint.h>/<stddef.h> (no libc, no malloc, no
 * floating point). EVERY write to `out` and `scratch` is bounded against its
 * cap — this runs in a kernel with no stack guard page, so an overflow is
 * memory corruption — and any overflow or bad arg returns -1.
 *
 * Byte orderings that are load-bearing (must mirror what png.c's decoder reads
 * and what RFC 1950/2083 mandate):
 *   - Chunk length and the four chunk fields (IHDR width/height) are big-endian
 *     u32. The CRC-32 trailer of each chunk is also big-endian and covers
 *     type+data (not the length).
 *   - The zlib header is 0x78 0x01 (CMF=0x78: CM=8 deflate, CINFO=7 -> 32 KB
 *     window; FLG=0x01: FCHECK so (CMF*256+FLG)=0x7801 is a multiple of 31, no
 *     preset dict, fastest level). The decoder simply skips these 2 bytes.
 *   - The zlib Adler-32 trailer is big-endian and is computed over the
 *     *uncompressed* (filtered) data.
 */
#include "png.h"
#include "inflate.h"   /* raw_deflate() */
#include <stddef.h>

/* ------------------------------------------------------------------ CRC-32 */
/* Standard reflected CRC-32 (polynomial 0xEDB88320, as used by gzip/zlib/PNG),
 * built once into a 256-entry table on first use. Identical algorithm to
 * deflate.c's crc32_buf, kept local so this file is self-contained. */
static uint32_t pe_crc_tab[256];
static int      pe_crc_ready = 0;

static void pe_crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        pe_crc_tab[n] = c;
    }
    pe_crc_ready = 1;
}

/* CRC-32 accumulated across calls so a chunk's type and data can be fed
 * separately. Caller seeds with 0xFFFFFFFF and finalises by XOR 0xFFFFFFFF. */
static uint32_t pe_crc_update(uint32_t c, const uint8_t *p, int len) {
    if (!pe_crc_ready) pe_crc_init();
    for (int i = 0; i < len; i++)
        c = pe_crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

/* ----------------------------------------------------------------- Adler-32 */
/* RFC 1950 §9: s1 = 1 + sum(bytes), s2 = sum(s1), both mod 65521; the checksum
 * is (s2 << 16) | s1. 65521 is the largest prime below 65536. We reduce inside
 * the loop (every 4096 bytes is comfortably safe against 32-bit overflow:
 * s2 grows by at most ~n*65520, so reducing well before 2^32) to stay in
 * uint32_t with no division-heavy inner loop. */
#define ADLER_MOD 65521u
static uint32_t pe_adler32(const uint8_t *p, long len) {
    uint32_t s1 = 1, s2 = 0;
    long i = 0;
    while (i < len) {
        long chunk = len - i;
        if (chunk > 4096) chunk = 4096;
        for (long j = 0; j < chunk; j++) {
            s1 += p[i + j];
            s2 += s1;
        }
        s1 %= ADLER_MOD;
        s2 %= ADLER_MOD;
        i += chunk;
    }
    return (s2 << 16) | s1;
}

/* --------------------------------------------------- bounded output writer */
/* A cursor into out[0..cap) that drops (and flags) any write past cap. The
 * caller turns a set `oom` into a -1 return — nothing is ever written OOB. */
typedef struct { uint8_t *out; int cap; int pos; int oom; } pe_w;

static void pe_put(pe_w *w, uint8_t b) {
    if (w->pos >= w->cap) { w->oom = 1; return; }
    w->out[w->pos++] = b;
}
static void pe_put_be32(pe_w *w, uint32_t v) {
    pe_put(w, (uint8_t)(v >> 24));
    pe_put(w, (uint8_t)(v >> 16));
    pe_put(w, (uint8_t)(v >> 8));
    pe_put(w, (uint8_t)(v));
}
static void pe_put_bytes(pe_w *w, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) pe_put(w, p[i]);
}

/* Write one chunk: length(4 BE) + type(4) + data + CRC32(4 BE), where the CRC
 * covers type+data (RFC 2083 §5.3). All writes go through the bounded cursor. */
static void pe_chunk(pe_w *w, const char type[4], const uint8_t *data, int dlen) {
    uint8_t t[4] = { (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3] };
    pe_put_be32(w, (uint32_t)dlen);
    pe_put_bytes(w, t, 4);
    pe_put_bytes(w, data, dlen);
    uint32_t crc = pe_crc_update(0xFFFFFFFFu, t, 4);
    crc = pe_crc_update(crc, data, dlen);
    pe_put_be32(w, crc ^ 0xFFFFFFFFu);
}

/* --------------------------------------------------------------- encoder */
/*
 * Encode w*h RGB pixels (`rgb`, 3 bytes/pixel, row-major top-to-bottom) into a
 * complete PNG file in out[0..outcap). `scratch` holds the filtered scanlines
 * to compress and needs (1 + w*3)*h bytes. Returns total PNG bytes written, or
 * -1 on bad args (w<=0, h<=0) or any overflow (outcap or scratchcap too small).
 */
int png_encode(const uint8_t *rgb, int w, int h,
               uint8_t *out, int outcap,
               uint8_t *scratch, int scratchcap) {
    if (w <= 0 || h <= 0) return -1;
    if (!rgb || !out || !scratch) return -1;

    /* Bound dimensions so every size below stays well within 32-bit `int` (the
     * chunk-length and raw_deflate APIs are int-typed) and 64-bit `long`
     * intermediates can't wrap: 32768*3 fits an int, and (1+w*3)*h with both
     * <= 32768 fits comfortably in a long. A crafted huge w/h would otherwise
     * let a length computation wrap and defeat the cap checks below. */
    if (w > 32768 || h > 32768) return -1;

    long rowbytes = (long)w * 3;             /* RGB bytes per scanline */
    long stride   = rowbytes + 1;            /* + the leading filter byte */
    long filtered = stride * (long)h;        /* size of the pre-compression buffer */
    if (filtered > scratchcap) return -1;    /* scratch too small */
    if (filtered > 0x7FFFFFFF) return -1;     /* keep it inside int for raw_deflate */

    /* Build the filtered buffer: each scanline is [0x00 filter][row RGB]. Every
     * store is within scratch[0..filtered) <= scratch[0..scratchcap). */
    for (long y = 0; y < h; y++) {
        uint8_t *dst = scratch + y * stride;
        const uint8_t *src = rgb + y * rowbytes;
        dst[0] = 0;                          /* filter type 0 = None */
        for (long x = 0; x < rowbytes; x++)
            dst[1 + x] = src[x];
    }

    /* Adler-32 over the *uncompressed* filtered data (RFC 1950 trailer). */
    uint32_t adler = pe_adler32(scratch, filtered);

    /* IHDR data: width, height (BE u32), bit depth 8, colour type 2 (RGB),
     * compression 0, filter 0, interlace 0. (RFC 2083 §4.1.1.) */
    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)((uint32_t)w >> 24); ihdr[1] = (uint8_t)((uint32_t)w >> 16);
    ihdr[2] = (uint8_t)((uint32_t)w >> 8);  ihdr[3] = (uint8_t)((uint32_t)w);
    ihdr[4] = (uint8_t)((uint32_t)h >> 24); ihdr[5] = (uint8_t)((uint32_t)h >> 16);
    ihdr[6] = (uint8_t)((uint32_t)h >> 8);  ihdr[7] = (uint8_t)((uint32_t)h);
    ihdr[8]  = 8;    /* bit depth */
    ihdr[9]  = 2;    /* colour type: truecolour RGB */
    ihdr[10] = 0;    /* compression method: deflate */
    ihdr[11] = 0;    /* filter method: adaptive (per-scanline filter byte) */
    ihdr[12] = 0;    /* interlace method: none */

    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    pe_w w_out = { out, outcap, 0, 0 };

    /* Signature + IHDR. */
    pe_put_bytes(&w_out, sig, 8);
    pe_chunk(&w_out, "IHDR", ihdr, 13);

    /* IDAT: zlib header (0x78 0x01) + DEFLATE body + Adler-32 (BE). We compress
     * directly into the output stream after the chunk's length+type+zlib-header,
     * so the DEFLATE body's bound IS the remaining output space; raw_deflate
     * returns -1 if it would overflow, and we then bail.
     *
     * Layout at idat_pos: [len:4][type:4][0x78 0x01][deflate body][adler:4].
     * We can't know `len` until the body is compressed, so reserve the 4-byte
     * length, write type + zlib header, compress into the gap, append Adler-32,
     * then patch the length and CRC. The CRC covers type + the whole zlib data.
     */
    int idat_pos = w_out.pos;                /* start of this chunk's length field */
    pe_put_be32(&w_out, 0);                  /* placeholder length, patched below */
    int type_pos = w_out.pos;
    pe_put(&w_out, 'I'); pe_put(&w_out, 'D'); pe_put(&w_out, 'A'); pe_put(&w_out, 'T');
    pe_put(&w_out, 0x78);                    /* zlib CMF */
    pe_put(&w_out, 0x01);                    /* zlib FLG */
    if (w_out.oom) return -1;                /* not even room for the header */

    /* Compress the filtered buffer straight into the remaining output. */
    int body_cap = w_out.cap - w_out.pos;    /* >= 0 since pos <= cap */
    int body = raw_deflate(scratch, (int)filtered, w_out.out + w_out.pos, body_cap);
    if (body < 0) return -1;                 /* output overflow inside DEFLATE */
    w_out.pos += body;

    /* Adler-32 trailer (big-endian), completing the zlib stream. */
    pe_put_be32(&w_out, adler);
    if (w_out.oom) return -1;

    /* zlib stream = 2 (header) + body + 4 (adler); IDAT data = zlib stream. */
    int idat_dlen = 2 + body + 4;

    /* Patch the IDAT length field (BE u32) now that the body size is known. */
    out[idat_pos + 0] = (uint8_t)((uint32_t)idat_dlen >> 24);
    out[idat_pos + 1] = (uint8_t)((uint32_t)idat_dlen >> 16);
    out[idat_pos + 2] = (uint8_t)((uint32_t)idat_dlen >> 8);
    out[idat_pos + 3] = (uint8_t)((uint32_t)idat_dlen);

    /* IDAT CRC over type + data (the 4 type bytes then the zlib stream). */
    uint32_t crc = pe_crc_update(0xFFFFFFFFu, out + type_pos, 4 + idat_dlen);
    crc ^= 0xFFFFFFFFu;
    pe_put_be32(&w_out, crc);
    if (w_out.oom) return -1;

    /* IEND: zero-length chunk. */
    pe_chunk(&w_out, "IEND", (const uint8_t *)0, 0);
    if (w_out.oom) return -1;

    return w_out.pos;
}
