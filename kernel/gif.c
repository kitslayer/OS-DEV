/*
 * gif.c — a minimal GIF decoder (GIF87a/89a, first frame only).
 *
 * GIF compresses palette indices with variable-width LZW. This decodes the
 * first image block into palette indices (handling the clear/end codes, the
 * dictionary growth, the KwKwK self-referential case, and de-interlacing),
 * then expands them to RGBA via the active colour table, honouring a tRNS-style
 * transparent index from a Graphic Control Extension. Standalone (only
 * <stdint.h>) so it is unit-testable on the host.
 *
 * Inputs may be untrusted, so every table index and buffer write is bounded.
 */
#include "gif.h"

/* LZW work tables (decoder runs serially on the WM thread; static is fine). */
static uint16_t lzw_prefix[4096];
static uint8_t  lzw_suffix[4096];
static uint8_t  lzw_stack[4096];

/* A bit reader over GIF's chain of length-prefixed sub-blocks (LSB first). */
typedef struct {
    const uint8_t *data; int len, pos;
    int sub_remaining;
    uint32_t bitbuf; int bitcnt;
} bitreader;

static int next_byte(bitreader *br) {
    while (br->sub_remaining == 0) {
        if (br->pos >= br->len) return -1;
        int blen = br->data[br->pos++];
        if (blen == 0) return -1;                 /* block terminator */
        br->sub_remaining = blen;
    }
    if (br->pos >= br->len) return -1;
    br->sub_remaining--;
    return br->data[br->pos++];
}
static int get_code(bitreader *br, int code_size) {
    while (br->bitcnt < code_size) {
        int b = next_byte(br);
        if (b < 0) return -1;
        br->bitbuf |= (uint32_t)b << br->bitcnt;
        br->bitcnt += 8;
    }
    int code = br->bitbuf & ((1u << code_size) - 1);
    br->bitbuf >>= code_size;
    br->bitcnt -= code_size;
    return code;
}

/* Decode the LZW image data into `idx` (one palette index per pixel).
 * Returns the number of indices produced, or -1. */
static int lzw_decode(bitreader *br, int min_code_size, uint8_t *idx, int idx_cap) {
    if (min_code_size < 2 || min_code_size > 8) return -1;
    int clear = 1 << min_code_size, end = clear + 1;
    int code_size = min_code_size + 1, next = end + 1, old = -1;
    uint8_t first = 0;
    int out = 0;
    for (;;) {
        int code = get_code(br, code_size);
        if (code < 0) break;
        if (code == clear) { code_size = min_code_size + 1; next = end + 1; old = -1; continue; }
        if (code == end) break;
        if (old < 0) {                                  /* first code after clear */
            if (code >= clear) return -1;               /* must be a literal */
            if (out < idx_cap) idx[out++] = (uint8_t)code;
            first = (uint8_t)code; old = code; continue;
        }
        int incode = code, sp = 0;
        if (code >= next) { lzw_stack[sp++] = first; code = old; }   /* KwKwK */
        while (code >= clear) {                          /* walk prefixes to a literal */
            if (code >= 4096 || sp >= 4096) return -1;
            lzw_stack[sp++] = lzw_suffix[code];
            code = lzw_prefix[code];
        }
        if (sp >= 4096) return -1;
        lzw_stack[sp++] = (uint8_t)code;
        first = (uint8_t)code;
        while (sp > 0) { sp--; if (out < idx_cap) idx[out++] = lzw_stack[sp]; }
        if (next < 4096) {                               /* add prev+first to the dictionary */
            lzw_prefix[next] = (uint16_t)old;
            lzw_suffix[next] = first;
            next++;
            if (next == (1 << code_size) && code_size < 12) code_size++;
        }
        old = incode;
    }
    return out;
}

static int rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

int gif_decode(const uint8_t *data, int len, uint8_t *out, int out_cap,
               uint8_t *scratch, int scratch_cap, int *w, int *h) {
    if (len < 13 || out_cap < 0 || scratch_cap < 0) return -1;
    if (!(data[0]=='G'&&data[1]=='I'&&data[2]=='F'&&data[3]=='8')) return -1;

    int p = 6;                                          /* after "GIF8?a" */
    /* Logical Screen Descriptor */
    int packed = data[p+4];
    int gct = packed & 0x80, gct_size = 1 << ((packed & 7) + 1);
    p += 7;
    const uint8_t *gctab = 0;
    if (gct) { if (gct_size*3 > len - p) return -1; gctab = data + p; p += gct_size*3; }  /* no p+... overflow */

    int trans = -1;                                     /* transparent index, if any */

    for (;;) {
        if (p >= len) return -1;
        int b = data[p++];
        if (b == 0x3B) return -1;                        /* trailer before an image */
        if (b == 0x21) {                                 /* extension */
            if (p >= len) return -1;
            int label = data[p++];
            if (label == 0xF9 && p + 4 < len && data[p] >= 4) {   /* Graphic Control */
                int gpacked = data[p+1];
                if (gpacked & 1) trans = data[p+4];      /* transparent colour index */
            }
            while (p < len) { int s = data[p++]; if (!s) break; p += s; }  /* skip sub-blocks */
            continue;
        }
        if (b == 0x2C) break;                            /* image descriptor */
        return -1;                                        /* unknown block */
    }

    /* Image Descriptor (10 bytes incl the 0x2C we consumed) */
    if (p + 9 > len) return -1;
    int iw = rd16(data + p + 4), ih = rd16(data + p + 6);
    int ipacked = data[p + 8];
    p += 9;
    if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096 || (long)iw*ih > 4*1024*1024) return -1;

    const uint8_t *ctab = gctab; int ctab_size = gct_size;
    if (ipacked & 0x80) {                                /* local colour table */
        int lsz = 1 << ((ipacked & 7) + 1);
        if (lsz*3 > len - p) return -1;                  /* no p+... overflow */
        ctab = data + p; ctab_size = lsz; p += lsz*3;
    }
    if (!ctab) return -1;                                /* no palette */
    int interlace = ipacked & 0x40;

    if ((long)iw * ih > scratch_cap) return -1;          /* one index per pixel */
    if ((long)iw * ih * 4 > out_cap) return -1;

    if (p >= len) return -1;
    int min_code_size = data[p++];
    bitreader br = { data, len, p, 0, 0, 0 };
    int n = lzw_decode(&br, min_code_size, scratch, (int)((long)iw*ih));
    if (n < iw * ih) return -1;                          /* short / corrupt */

    /* Expand indices -> RGBA. Decoded rows are consumed in order; for an
     * interlaced image they're placed into the 4 interlace passes. */
    static const int istart[4] = {0,4,2,1}, istep[4] = {8,8,4,2};
    int npass = interlace ? 4 : 1, srow = 0;
    for (int pass = 0; pass < npass; pass++) {
        int start = interlace ? istart[pass] : 0;
        int step  = interlace ? istep[pass]  : 1;
        for (int dy = start; dy < ih && srow < ih; dy += step, srow++) {
            const uint8_t *irow = scratch + (long)srow * iw;
            uint8_t *o = out + (long)dy * iw * 4;
            for (int x = 0; x < iw; x++) {
                int ci = irow[x];
                uint8_t r = 0, g = 0, bl = 0, al = 255;
                if (ci < ctab_size) { r = ctab[ci*3]; g = ctab[ci*3+1]; bl = ctab[ci*3+2]; }
                if (ci == trans) al = 0;
                o[x*4+0] = r; o[x*4+1] = g; o[x*4+2] = bl; o[x*4+3] = al;
            }
        }
    }
    *w = iw; *h = ih;
    return 0;
}

/* ---- animated GIF: decode up to `max_frames` composited frames ----
 * Each frame is a full logical-screen RGBA image written to out[frame*W*H*4],
 * built by compositing successive sub-images onto a canvas (honouring the
 * transparent index and disposal methods 0/1 = leave, 2 = restore-to-background).
 * `delays_cs` receives each frame's delay in centiseconds. Returns the frame
 * count (>=1), or -1. `scratch` must hold the canvas (W*H*4) + an index buffer. */
int gif_decode_anim(const uint8_t *data, int len, uint8_t *out, int out_cap,
                    uint8_t *scratch, int scratch_cap, int *w, int *h,
                    int *delays_cs, int max_frames) {
    if (len < 13 || max_frames < 1) return -1;
    if (!(data[0]=='G'&&data[1]=='I'&&data[2]=='F'&&data[3]=='8')) return -1;

    int p = 6;
    int W = rd16(data + p), H = rd16(data + p + 2);
    int packed = data[p+4];
    int gct = packed & 0x80, gct_size = 1 << ((packed & 7) + 1);
    p += 7;
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096 || (long)W*H > 2*1024*1024) return -1;
    const uint8_t *gctab = 0;
    if (gct) { if (gct_size*3 > len - p) return -1; gctab = data + p; p += gct_size*3; }

    long frame_px = (long)W * H;
    /* scratch layout: [canvas RGBA W*H*4][index buffer W*H] */
    if (frame_px * 4 + frame_px > scratch_cap) return -1;
    uint8_t *canvas = scratch;
    uint8_t *idx    = scratch + frame_px * 4;
    for (long i = 0; i < frame_px * 4; i += 4) { canvas[i]=canvas[i+1]=canvas[i+2]=255; canvas[i+3]=0; }

    int trans = -1, delay = 0, disposal = 0;
    int frame = 0;
    int prev_l = 0, prev_t = 0, prev_w = 0, prev_h = 0, prev_disp = 0;

    for (;;) {
        if (p >= len) break;
        int b = data[p++];
        if (b == 0x3B) break;                            /* trailer */
        if (b == 0x21) {                                 /* extension */
            if (p >= len) break;
            int label = data[p++];
            if (label == 0xF9 && p + 4 < len && data[p] >= 4) {
                int gpacked = data[p+1];
                delay = rd16(data + p + 2);
                disposal = (gpacked >> 2) & 7;
                trans = (gpacked & 1) ? data[p+4] : -1;
            }
            while (p < len) { int s = data[p++]; if (!s) break; if (p + s > len) return -1; p += s; }
            continue;
        }
        if (b != 0x2C) return -1;                         /* unknown block */

        if (p + 9 > len) return -1;
        int il = rd16(data+p), it = rd16(data+p+2), iw = rd16(data+p+4), ih = rd16(data+p+6);
        int ipacked = data[p+8];
        p += 9;
        if (iw <= 0 || ih <= 0 || il < 0 || it < 0 || il + iw > W || it + ih > H) return -1;

        const uint8_t *ctab = gctab; int ctab_size = gct_size;
        if (ipacked & 0x80) {
            int lsz = 1 << ((ipacked & 7) + 1);
            if (lsz*3 > len - p) return -1;
            ctab = data + p; ctab_size = lsz; p += lsz*3;
        }
        if (!ctab) return -1;
        int interlace = ipacked & 0x40;

        /* apply the PREVIOUS frame's disposal to the canvas */
        if (frame > 0 && prev_disp == 2) {               /* restore-to-background: clear prev rect */
            for (int y = prev_t; y < prev_t + prev_h; y++)
                for (int x = prev_l; x < prev_l + prev_w; x++) {
                    uint8_t *o = canvas + ((long)y * W + x) * 4;
                    o[0]=o[1]=o[2]=255; o[3]=0;
                }
        }

        if (p >= len) return -1;
        int min_code_size = data[p++];
        bitreader br = { data, len, p, 0, 0, 0 };
        int n = lzw_decode(&br, min_code_size, idx, (int)((long)iw*ih));
        if (n < iw * ih) return -1;
        p = br.pos;                                      /* advance past this frame's LZW data */
        while (p < len && data[p] != 0) { int s = data[p]; if (p + 1 + s > len) break; p += 1 + s; }
        if (p < len && data[p] == 0) p++;                /* sub-block terminator */

        /* composite this sub-image onto the canvas (skip transparent pixels) */
        static const int istart[4] = {0,4,2,1}, istep[4] = {8,8,4,2};
        int npass = interlace ? 4 : 1, srow = 0;
        for (int pass = 0; pass < npass; pass++) {
            int start = interlace ? istart[pass] : 0, step = interlace ? istep[pass] : 1;
            for (int dy = start; dy < ih && srow < ih; dy += step, srow++) {
                const uint8_t *irow = idx + (long)srow * iw;
                for (int x = 0; x < iw; x++) {
                    int ci = irow[x];
                    if (ci == trans) continue;           /* transparent: keep canvas pixel */
                    uint8_t *o = canvas + ((long)(it + dy) * W + (il + x)) * 4;
                    o[0] = (ci < ctab_size) ? ctab[ci*3]   : 0;
                    o[1] = (ci < ctab_size) ? ctab[ci*3+1] : 0;
                    o[2] = (ci < ctab_size) ? ctab[ci*3+2] : 0;
                    o[3] = 255;
                }
            }
        }

        /* snapshot the canvas as this frame */
        if ((long)(frame + 1) * frame_px * 4 > out_cap) break;   /* out of room */
        for (long i = 0; i < frame_px * 4; i++) out[(long)frame * frame_px * 4 + i] = canvas[i];
        if (delays_cs) delays_cs[frame] = delay;
        prev_l = il; prev_t = it; prev_w = iw; prev_h = ih; prev_disp = disposal;
        frame++;
        trans = -1; delay = 0; disposal = 0;             /* reset for next GCE */
        if (frame >= max_frames) break;
    }

    if (frame < 1) return -1;
    *w = W; *h = H;
    return frame;
}
