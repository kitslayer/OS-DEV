/*
 * jpeg.c — a from-scratch baseline (sequential DCT) JPEG decoder.
 *
 * Decodes a baseline JPEG (SOF0, 8-bit, Huffman) into RGBA, matching the PNG/GIF
 * decoders' interface so the browser can blit it the same way. Handles 4:4:4,
 * 4:2:2 and 4:2:0 chroma subsampling and restart markers.
 *
 * The kernel builds with -mgeneral-regs-only (no FPU/SSE), so EVERYTHING here is
 * integer fixed-point — in particular the IDCT, which uses the well-known
 * Loeffler integer constants (the same ones NanoJPEG/the IJG "islow" path use).
 *
 * No dynamic allocation: the caller passes an `out` RGBA buffer and a `scratch`
 * buffer (used for the per-component sample planes). It is host-unit-tested in
 * tools/ against libjpeg/Pillow output. Untrusted input: every read is bounded.
 */
#include "jpeg.h"
#include "string.h"

#define JE_OK   0
#define JE_ERR (-1)

typedef struct {
    const uint8_t *bits;     /* Huffman: 16 code-length counts */
    const uint8_t *vals;     /* Huffman symbol values          */
    int            nvals;
    /* fast lookup: for each 1..16-bit prefix we store (len<<8)|sym when resolved */
    int            maxcode[18];
    int            valptr[18];
    int            mincode[18];
} huff_t;

typedef struct {
    int id, hs, vs, tq;      /* component id, h/v sampling, quant table */
    int td, ta;              /* DC/AC huffman table selectors (from SOS) */
    long dcpred;             /* DC predictor (64-bit: dcpred*quant must not overflow) */
    uint8_t *plane;          /* decoded samples, stride = bx*8 */
    int bx, by;              /* blocks per component, MCU-padded (mcux*hs, mcuy*vs) */
    int wbx, wby;            /* blocks covering the real image (non-interleaved scans) */
    int px, py;              /* plane pixel dims (bx*8, by*8) */
} comp_t;

typedef struct {
    const uint8_t *d; int len, p;       /* input */
    int W, H, ncomp;
    comp_t comp[3];
    uint16_t qt[4][64];                 /* quant tables (natural order) */
    huff_t  hdc[4], hac[4];
    int     hmax, vmax;                 /* max sampling factors */
    int     mcux, mcuy;                 /* MCUs across/down */
    int     ri;                         /* restart interval */
    /* bit reader over entropy-coded data */
    uint32_t bitbuf; int bitcnt; int eod;
    /* progressive (SOF2) state */
    int      progressive;
    int16_t *coef[3];                   /* full per-component coefficient buffers */
    int      eobrun;                    /* end-of-band run, persists across blocks in a scan */
    int      Ss, Se, Ah, Al;            /* current scan: spectral band + approximation bits */
    int      scan_n;                    /* components in the current scan */
    int      scan_c[3];                 /* their component indices */
} jctx;

/* zig-zag order: index in the 8x8 block for the k-th coefficient in the stream */
static const uint8_t ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

static int clampb(long x) { return x < 0 ? 0 : (x > 255 ? 255 : (int)x); }

/* ---------------- bit reader (handles FF00 stuffing + restart markers) ------ */

static int next_byte(jctx *j) {
    if (j->p >= j->len) { j->eod = 1; return 0; }
    uint8_t b = j->d[j->p++];
    if (b == 0xFF) {
        uint8_t m = (j->p < j->len) ? j->d[j->p] : 0xD9;
        if (m == 0x00) { j->p++; return 0xFF; }      /* stuffed byte */
        /* a real marker (restart / EOI): stop feeding entropy bits */
        j->eod = 1; j->p--;                          /* leave the FF for the caller */
        return 0;
    }
    return b;
}

static int get_bit(jctx *j) {
    if (j->bitcnt == 0) {
        if (j->eod) return 0;
        j->bitbuf = (uint32_t)next_byte(j);
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return (j->bitbuf >> j->bitcnt) & 1;
}

static int get_bits(jctx *j, int n) {
    int v = 0;
    while (n-- > 0) v = (v << 1) | get_bit(j);
    return v;
}

/* sign-extend an n-bit magnitude per the JPEG "receive/extend" rule */
static int extend(int v, int n) {
    return (v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

/* ---------------- Huffman ---------------------------------------------------- */

static void huff_build(huff_t *h) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        int n = h->bits[l - 1];
        if (n == 0) { h->maxcode[l] = -1; h->valptr[l] = 0; h->mincode[l] = 0; }
        else {
            h->valptr[l] = k;
            h->mincode[l] = code;
            code += n; k += n;
            h->maxcode[l] = code - 1;
        }
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->nvals = k;
}

static int huff_decode(jctx *j, huff_t *h) {
    if (!h->bits) return -1;          /* table referenced but never defined (DHT missing) */
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | get_bit(j);
        if (h->bits[l - 1] && code <= h->maxcode[l]) {
            int idx = h->valptr[l] + (code - h->mincode[l]);
            if (idx < 0 || idx >= h->nvals) return -1;
            return h->vals[idx];
        }
    }
    return -1;
}

/* ---------------- integer IDCT (Loeffler constants, row then column) --------- */

#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static void idct_row(long *b) {
    long x0,x1,x2,x3,x4,x5,x6,x7,x8;
    /* multiply (not <<) signed coefficients: left-shift of a negative is UB */
    if (!((x1 = b[4] * 2048) | (x2 = b[6]) | (x3 = b[2]) |
          (x4 = b[1]) | (x5 = b[7]) | (x6 = b[5]) | (x7 = b[3]))) {
        b[0]=b[1]=b[2]=b[3]=b[4]=b[5]=b[6]=b[7] = b[0] * 8;
        return;
    }
    x0 = (b[0] * 2048) + 128;
    x8 = W7 * (x4 + x5);
    x4 = x8 + (W1 - W7) * x4;
    x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);
    x6 = x8 - (W3 - W5) * x6;
    x7 = x8 - (W3 + W5) * x7;
    x8 = x0 + x1; x0 -= x1;
    x1 = W6 * (x3 + x2);
    x2 = x1 - (W2 + W6) * x2;
    x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6; x4 -= x6;
    x6 = x5 + x7; x5 -= x7;
    x7 = x8 + x3; x8 -= x3;
    x3 = x0 + x2; x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    b[0] = (x7 + x1) >> 8; b[1] = (x3 + x2) >> 8;
    b[2] = (x0 + x4) >> 8; b[3] = (x8 + x6) >> 8;
    b[4] = (x8 - x6) >> 8; b[5] = (x0 - x4) >> 8;
    b[6] = (x3 - x2) >> 8; b[7] = (x7 - x1) >> 8;
}

static void idct_col(const long *b, uint8_t *out, int stride) {
    long x0,x1,x2,x3,x4,x5,x6,x7,x8;
    if (!((x1 = b[8*4] * 256) | (x2 = b[8*6]) | (x3 = b[8*2]) |
          (x4 = b[8*1]) | (x5 = b[8*7]) | (x6 = b[8*5]) | (x7 = b[8*3]))) {
        x0 = clampb(((b[0] + 32) >> 6) + 128);
        for (int k = 0; k < 8; k++) out[k * stride] = (uint8_t)x0;
        return;
    }
    x0 = (b[0] * 256) + 8192;
    x8 = W7 * (x4 + x5) + 4;
    x4 = (x8 + (W1 - W7) * x4) >> 3;
    x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;
    x6 = (x8 - (W3 - W5) * x6) >> 3;
    x7 = (x8 - (W3 + W5) * x7) >> 3;
    x8 = x0 + x1; x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;
    x2 = (x1 - (W2 + W6) * x2) >> 3;
    x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6; x4 -= x6;
    x6 = x5 + x7; x5 -= x7;
    x7 = x8 + x3; x8 -= x3;
    x3 = x0 + x2; x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    out[0]        = (uint8_t)clampb(((x7 + x1) >> 14) + 128);
    out[stride]   = (uint8_t)clampb(((x3 + x2) >> 14) + 128);
    out[stride*2] = (uint8_t)clampb(((x0 + x4) >> 14) + 128);
    out[stride*3] = (uint8_t)clampb(((x8 + x6) >> 14) + 128);
    out[stride*4] = (uint8_t)clampb(((x8 - x6) >> 14) + 128);
    out[stride*5] = (uint8_t)clampb(((x0 - x4) >> 14) + 128);
    out[stride*6] = (uint8_t)clampb(((x3 - x2) >> 14) + 128);
    out[stride*7] = (uint8_t)clampb(((x7 - x1) >> 14) + 128);
}

/* Inverse-DCT a dequantized 8x8 block (natural order) into samples at `out`. */
static void idct_block(long *blk, uint8_t *out, int stride) {
    for (int i = 0; i < 8; i++) idct_row(blk + i * 8);
    for (int i = 0; i < 8; i++) idct_col(blk + i, out + i, stride);
}

/* Decode one 8x8 block to spatial samples written into `out` (stride). */
static int decode_block(jctx *j, comp_t *c, uint8_t *out, int stride) {
    long blk[64];
    memset(blk, 0, sizeof(blk));
    /* DC */
    int t = huff_decode(j, &j->hdc[c->td]);
    if (t < 0 || t > 16) return JE_ERR;
    int diff = t ? extend(get_bits(j, t), t) : 0;
    c->dcpred += diff;
    /* clamp the predictor: valid 8-bit DC is tiny (±2047); this bounds an
     * adversarial DC run so dcpred*quant and the IDCT can't overflow 64 bits. */
    if (c->dcpred >  (1L << 20)) c->dcpred =  (1L << 20);
    if (c->dcpred < -(1L << 20)) c->dcpred = -(1L << 20);
    blk[0] = c->dcpred * (long)j->qt[c->tq][0];
    /* AC */
    int k = 1;
    while (k < 64) {
        int rs = huff_decode(j, &j->hac[c->ta]);
        if (rs < 0) return JE_ERR;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r != 15) break;          /* EOB */
            k += 16;                     /* ZRL: skip 16 zeros */
        } else {
            k += r;
            if (k >= 64) break;
            int coeff = extend(get_bits(j, s), s);
            blk[ZZ[k]] = (long)coeff * j->qt[c->tq][ZZ[k]];
            k++;
        }
    }
    idct_block(blk, out, stride);
    return JE_OK;
}

/* ---------------- marker parsing -------------------------------------------- */

static int rd16(jctx *j) { int v = (j->d[j->p] << 8) | j->d[j->p + 1]; j->p += 2; return v; }

static int parse_dqt(jctx *j, int len) {
    int end = j->p + len;
    while (j->p < end) {
        int pq_tq = j->d[j->p++];
        int pq = pq_tq >> 4, tq = pq_tq & 15;
        if (tq > 3) return JE_ERR;
        if (j->p + (pq ? 128 : 64) > end) return JE_ERR;   /* table must fit the segment */
        for (int k = 0; k < 64; k++) {
            int v = pq ? rd16(j) : j->d[j->p++];
            j->qt[tq][ZZ[k]] = (uint16_t)v;       /* store in natural order */
        }
    }
    return JE_OK;
}

static int parse_sof(jctx *j, int len) {
    int end = j->p + len;
    if (j->p + 6 > end) return JE_ERR;
    int prec = j->d[j->p++];
    if (prec != 8) return JE_ERR;
    j->H = rd16(j); j->W = rd16(j);
    j->ncomp = j->d[j->p++];
    if (j->ncomp != 1 && j->ncomp != 3) return JE_ERR;
    if (j->W <= 0 || j->H <= 0 || j->W > 4096 || j->H > 4096) return JE_ERR;
    if (j->p + j->ncomp * 3 > end) return JE_ERR;
    j->hmax = j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        comp_t *c = &j->comp[i];
        c->id = j->d[j->p++];
        int hv = j->d[j->p++];
        c->hs = hv >> 4; c->vs = hv & 15;
        c->tq = j->d[j->p++];
        if (c->hs < 1 || c->hs > 2 || c->vs < 1 || c->vs > 2 || c->tq > 3) return JE_ERR;
        if (c->hs > j->hmax) j->hmax = c->hs;
        if (c->vs > j->vmax) j->vmax = c->vs;
    }
    return JE_OK;
}

static int parse_dht(jctx *j, int len) {
    int end = j->p + len;
    while (j->p < end) {
        int tc_th = j->d[j->p++];
        int tc = tc_th >> 4, th = tc_th & 15;
        if (th > 3) return JE_ERR;
        huff_t *h = tc ? &j->hac[th] : &j->hdc[th];
        if (j->p + 16 > end) return JE_ERR;          /* the 16 length counts must fit */
        h->bits = &j->d[j->p];
        int total = 0;
        for (int l = 0; l < 16; l++) total += j->d[j->p + l];
        j->p += 16;
        if (total > 256 || j->p + total > end) return JE_ERR;
        h->vals = &j->d[j->p];
        j->p += total;
        huff_build(h);
    }
    return JE_OK;
}

static int parse_sos(jctx *j, int len) {
    int end = j->p + len;
    if (j->p + 1 > end) return JE_ERR;
    int ns = j->d[j->p++];
    /* baseline: all components in one scan. progressive: 1..ncomp per scan. */
    if (ns < 1 || ns > j->ncomp) return JE_ERR;
    if (j->p + ns * 2 + 3 > end) return JE_ERR;
    j->scan_n = ns;
    for (int i = 0; i < ns; i++) {
        int cid = j->d[j->p++];
        int t = j->d[j->p++];
        int ci = -1;
        for (int k = 0; k < j->ncomp; k++) if (j->comp[k].id == cid) ci = k;
        if (ci < 0) return JE_ERR;
        comp_t *c = &j->comp[ci];
        c->td = t >> 4; c->ta = t & 15;
        if (c->td > 3 || c->ta > 3) return JE_ERR;
        j->scan_c[i] = ci;
    }
    j->Ss = j->d[j->p++];
    j->Se = j->d[j->p++];
    int ahal = j->d[j->p++];
    j->Ah = ahal >> 4; j->Al = ahal & 15;
    if (j->Ss > 63 || j->Se > 63 || j->Ss > j->Se) { j->Ss = 0; j->Se = 63; }  /* sane defaults */
    if (j->Ah > 13 || j->Al > 13) return JE_ERR;     /* point transform is capped at 13 */
    return JE_OK;
}

/* ---------------- top-level decode ------------------------------------------ */

/* Peek dimensions + the exact scratch size jpeg_decode will need, without
 * decoding — so the caller can size buffers. Returns 0 + sets w/h/scratch_needed,
 * or <0. Mirrors the plane layout computed in jpeg_decode. */
int jpeg_probe(const uint8_t *data, int len, int *w, int *h, long *scratch_needed) {
    if (len < 2 || data[0] != 0xFF || data[1] != 0xD8) return JE_ERR;
    int p = 2;
    while (p + 4 <= len) {
        if (data[p] != 0xFF) { p++; continue; }
        int m = data[p + 1]; p += 2;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) return JE_ERR;
        if (p + 2 > len) return JE_ERR;
        int sl = (data[p] << 8) | data[p + 1];
        if (sl < 2 || p + sl > len) return JE_ERR;
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {    /* SOF0 / SOF1 / SOF2(progressive) */
            int prog = (m == 0xC2);
            int q = p + 2;
            if (q + 6 > len) return JE_ERR;
            int H = (data[q + 1] << 8) | data[q + 2];
            int W = (data[q + 3] << 8) | data[q + 4];
            int nc = data[q + 5];
            if (W <= 0 || H <= 0 || nc < 1 || nc > 3) return JE_ERR;
            if (q + 6 + nc * 3 > len) return JE_ERR;
            int hmax = 1, vmax = 1, hs[3], vs[3];
            for (int i = 0; i < nc; i++) {
                int hv = data[q + 6 + i * 3 + 1];
                hs[i] = hv >> 4; vs[i] = hv & 15;
                if (hs[i] < 1 || hs[i] > 2 || vs[i] < 1 || vs[i] > 2) return JE_ERR;
                if (hs[i] > hmax) hmax = hs[i];
                if (vs[i] > vmax) vmax = vs[i];
            }
            int mcux = (W + 8 * hmax - 1) / (8 * hmax);
            int mcuy = (H + 8 * vmax - 1) / (8 * vmax);
            long need = 0;
            for (int i = 0; i < nc; i++) {
                int bx = mcux * hs[i], by = mcuy * vs[i];
                need += (long)(bx * 8) * (by * 8);                 /* sample plane */
                if (prog) need += (long)bx * by * 64 * 2;          /* coefficient buffer */
            }
            *w = W; *h = H; *scratch_needed = need;
            return JE_OK;
        }
        p += sl;
    }
    return JE_ERR;
}

/* Compute MCU grid + per-component block/plane dimensions (both the MCU-padded
 * bx/by and the real-image wbx/wby used by non-interleaved scans). */
static void setup_dims(jctx *j) {
    j->mcux = (j->W + 8 * j->hmax - 1) / (8 * j->hmax);
    j->mcuy = (j->H + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->ncomp; i++) {
        comp_t *c = &j->comp[i];
        c->bx = j->mcux * c->hs;  c->by = j->mcuy * c->vs;
        c->px = c->bx * 8;        c->py = c->by * 8;
        int cw = (j->W * c->hs + j->hmax - 1) / j->hmax;   /* this component's pixels */
        int ch = (j->H * c->vs + j->vmax - 1) / j->vmax;
        c->wbx = (cw + 7) / 8;    c->wby = (ch + 7) / 8;
    }
}

/* Progressive: lay out zeroed coefficient buffers + sample planes in scratch. */
static int setup_coef(jctx *j, uint8_t *scratch, int scratch_cap) {
    setup_dims(j);
    long total = 0;
    for (int i = 0; i < j->ncomp; i++)
        total += (long)j->comp[i].bx * j->comp[i].by * 64 * 2 + (long)j->comp[i].px * j->comp[i].py;
    if (total > scratch_cap) return JE_ERR;
    long off = 0;
    for (int i = 0; i < j->ncomp; i++) {
        long sz = (long)j->comp[i].bx * j->comp[i].by * 64 * 2;
        j->coef[i] = (int16_t *)(scratch + off);
        memset(scratch + off, 0, sz);
        off += sz;
    }
    for (int i = 0; i < j->ncomp; i++) {
        j->comp[i].plane = scratch + off;
        off += (long)j->comp[i].px * j->comp[i].py;
    }
    return JE_OK;
}

/* ---------------- progressive (SOF2) scan decoding -------------------------- */

/* One block's DC contribution for the current progressive scan. */
static int prog_dc(jctx *j, comp_t *c, int16_t *blk) {
    if (j->Ah == 0) {                              /* first DC scan */
        int t = huff_decode(j, &j->hdc[c->td]);
        if (t < 0 || t > 16) return JE_ERR;
        int diff = t ? extend(get_bits(j, t), t) : 0;
        c->dcpred += diff;
        if (c->dcpred >  (1L << 20)) c->dcpred =  (1L << 20);   /* clamp like baseline */
        if (c->dcpred < -(1L << 20)) c->dcpred = -(1L << 20);
        blk[0] = (int16_t)(c->dcpred * (1 << j->Al));   /* multiply: dcpred may be negative */
    } else {                                       /* DC refinement: add one bit */
        if (get_bit(j)) blk[0] |= (1 << j->Al);
    }
    return JE_OK;
}

/* One block's AC contribution for the current progressive scan (band Ss..Se). */
static int prog_ac(jctx *j, comp_t *c, int16_t *blk) {
    int Ss = j->Ss, Se = j->Se, bit = 1 << j->Al;
    if (j->Ah == 0) {                              /* AC first scan */
        if (j->eobrun > 0) { j->eobrun--; return JE_OK; }
        int k = Ss;
        while (k <= Se) {
            int rs = huff_decode(j, &j->hac[c->ta]);
            if (rs < 0) return JE_ERR;
            int r = rs >> 4, s = rs & 15;
            if (s == 0) {
                if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += get_bits(j, r); break; }
                k += 16;
            } else {
                k += r;
                if (k > Se) break;
                blk[ZZ[k]] = (int16_t)(extend(get_bits(j, s), s) * (1 << j->Al));
                k++;
            }
        }
    } else {                                       /* AC refinement scan */
        int k = Ss;
        if (j->eobrun > 0) {
            j->eobrun--;
            for (k = Ss; k <= Se; k++) {
                int16_t *p = &blk[ZZ[k]];
                if (*p != 0 && get_bit(j) && (*p & bit) == 0)
                    *p += (*p > 0) ? bit : -bit;
            }
            return JE_OK;
        }
        do {
            int rs = huff_decode(j, &j->hac[c->ta]);
            if (rs < 0) return JE_ERR;
            int r = rs >> 4, s = rs & 15;
            if (s == 0) {
                if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += get_bits(j, r); r = 64; }
                /* r==15: a run of 16 zero coefficients (refine nonzero ones en route) */
            } else {
                s = get_bit(j) ? bit : -bit;       /* new coefficient is ±1 unit */
            }
            while (k <= Se) {
                int16_t *p = &blk[ZZ[k++]];
                if (*p != 0) {
                    if (get_bit(j) && (*p & bit) == 0) *p += (*p > 0) ? bit : -bit;
                } else {
                    if (r == 0) { if (s) *p = (int16_t)s; break; }
                    r--;
                }
            }
        } while (k <= Se);
    }
    return JE_OK;
}

/* Discard remaining bits and skip the next RSTn marker; reset DC + EOB run. */
static void prog_restart(jctx *j) {
    j->bitcnt = 0; j->eod = 0;
    while (j->p + 1 < j->len && !(j->d[j->p] == 0xFF && j->d[j->p+1] >= 0xD0 && j->d[j->p+1] <= 0xD7))
        j->p++;
    if (j->p + 1 < j->len) j->p += 2;
    for (int i = 0; i < j->ncomp; i++) j->comp[i].dcpred = 0;
    j->eobrun = 0;
}

/* Decode one progressive scan into the coefficient buffers. */
static int decode_scan_prog(jctx *j) {
    j->bitcnt = 0; j->eod = 0;                  /* fresh entropy stream for this scan */
    j->eobrun = 0;
    for (int i = 0; i < j->scan_n; i++) j->comp[j->scan_c[i]].dcpred = 0;
    int isdc = (j->Ss == 0);
    /* spec: a DC band is exactly [0,0]; interleaved scans are DC-only */
    if (isdc && j->Se != 0) return JE_ERR;
    if (j->scan_n > 1 && !isdc) return JE_ERR;
    int restart = j->ri;

    if (j->scan_n == 1) {                           /* non-interleaved: component raster order */
        comp_t *c = &j->comp[j->scan_c[0]];
        for (int byi = 0; byi < c->wby; byi++) {
            for (int bxi = 0; bxi < c->wbx; bxi++) {
                int16_t *blk = j->coef[j->scan_c[0]] + ((long)byi * c->bx + bxi) * 64;
                if (isdc ? prog_dc(j, c, blk) : prog_ac(j, c, blk)) return JE_ERR;
                if (j->ri && --restart == 0) { prog_restart(j); restart = j->ri; }
            }
        }
    } else {                                        /* interleaved: MCU order (DC scans) */
        for (int my = 0; my < j->mcuy; my++) {
            for (int mx = 0; mx < j->mcux; mx++) {
                for (int s = 0; s < j->scan_n; s++) {
                    comp_t *c = &j->comp[j->scan_c[s]];
                    for (int by = 0; by < c->vs; by++)
                        for (int bx = 0; bx < c->hs; bx++) {
                            int bxi = mx * c->hs + bx, byi = my * c->vs + by;
                            int16_t *blk = j->coef[j->scan_c[s]] + ((long)byi * c->bx + bxi) * 64;
                            if (prog_dc(j, c, blk)) return JE_ERR;   /* interleaved is DC-only */
                        }
                }
                if (j->ri && --restart == 0) { prog_restart(j); restart = j->ri; }
            }
        }
    }
    return JE_OK;
}

/* Parallel YCbCr->RGB color conversion (M1533): a real ring-3 program can now
 * genuinely run threads across cores (M1531/M1532), so the final per-pixel
 * pass below — independent per pixel, reading only the fully-decoded (never
 * again written) component planes — is split across sys_clone'd threads
 * instead of running as one long serial loop. Guarded behind JPEG_THREADED
 * (set only for the ring-3 Makefile targets that actually use it) so this
 * file stays the portable, OS-independent, host-unit-testable decoder its own
 * header comment promises — the host img_test.c build never defines it and
 * gets the identical, unchanged sequential path.
 *
 * Thread-stack lifetime: intentionally LEAKED, never freed — the exact same
 * tradeoff user/webview.c's own task_create_stack wrapper already makes for
 * its fetch-worker thread. Freeing a stack the in-flight thread might still
 * be touching (the handful of instructions between it setting `done` and its
 * own sys_thread_exit() syscall actually trapping) is a real, if narrow, use-
 * after-free race — this codebase's established answer is "don't reclaim it,
 * let process exit do that," not "invent a tighter join primitive," and
 * imgdec.elf/imgview.elf are short-lived one-shot processes (scan the disk,
 * report, exit) so the bounded per-decode leak never accumulates across a
 * long-running session. */
#ifdef JPEG_THREADED
#include "ulib.h"

#define JPEG_MAX_THREADS      4
/* Gate on total PIXEL count, not just row count: a real sys_clone (task_create +
 * stack alloc + a scheduling round-trip) costs far more than converting a couple
 * thousand pixels, so a narrow-but-tall (or short-but-wide) image can still lose
 * to thread-spawn overhead if only rows are checked. 200K px (~500x400) is well
 * past where 3 spawns pay for themselves; this disk's own tiny 120x80 test JPEGs
 * (9.6K px) deliberately stay on the sequential path (verified in-guest, M1533). */
#define JPEG_CC_MIN_PIXELS    (200 * 1000)
#define JPEG_CC_STACK         (64 * 1024)

struct jpeg_cc_job { jctx *j; uint8_t *out; int y0, y1; volatile int done; };
#endif

static void jpeg_cc_range(jctx *j, uint8_t *out, int y0, int y1) {
    comp_t *Y = &j->comp[0];
    if (j->ncomp == 1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < j->W; x++) {
                int v = Y->plane[(long)y * Y->px + x];
                uint8_t *o = out + ((long)y * j->W + x) * 4;
                o[0] = o[1] = o[2] = (uint8_t)v; o[3] = 255;
            }
        return;
    }
    comp_t *Cb = &j->comp[1], *Cr = &j->comp[2];
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < j->W; x++) {
            int yy = Y->plane[(long)(y * Y->vs / j->vmax) * Y->px + (x * Y->hs / j->hmax)];
            int cbx = x * Cb->hs / j->hmax, cby = y * Cb->vs / j->vmax;
            int crx = x * Cr->hs / j->hmax, cry = y * Cr->vs / j->vmax;
            int cb = Cb->plane[(long)cby * Cb->px + cbx] - 128;
            int cr = Cr->plane[(long)cry * Cr->px + crx] - 128;
            int r = yy + ((91881 * cr) >> 16);
            int g = yy - ((22554 * cb + 46802 * cr) >> 16);
            int b = yy + ((116130 * cb) >> 16);
            uint8_t *o = out + ((long)y * j->W + x) * 4;
            o[0] = (uint8_t)clampb(r); o[1] = (uint8_t)clampb(g);
            o[2] = (uint8_t)clampb(b); o[3] = 255;
        }
    }
}

#ifdef JPEG_THREADED
static void jpeg_cc_thread(void *arg) {
    struct jpeg_cc_job *job = (struct jpeg_cc_job *)arg;
    jpeg_cc_range(job->j, job->out, job->y0, job->y1);
    job->done = 1;              /* LAST write before exiting -- the join loop waits on this */
    sys_thread_exit();
}

/* Split [0,H) across up to JPEG_MAX_THREADS-1 spawned threads + this calling
 * thread itself (which does the last chunk directly, avoiding a stack alloc
 * + spawn just to immediately wait on it). Falls back to the plain sequential
 * call for small images (JPEG_CC_MIN_PIXELS) or on malloc failure -- correctness
 * never depends on the thread count actually achieved. */
static void jpeg_color_convert(jctx *j, uint8_t *out) {
    int H = j->H;
    if ((long)j->W * H < JPEG_CC_MIN_PIXELS) { jpeg_cc_range(j, out, 0, H); return; }

    struct jpeg_cc_job jobs[JPEG_MAX_THREADS];
    int njobs = 0;
    int per = (H + JPEG_MAX_THREADS - 1) / JPEG_MAX_THREADS;
    for (int y0 = 0; y0 < H && njobs < JPEG_MAX_THREADS; y0 += per, njobs++) {
        int y1 = y0 + per; if (y1 > H) y1 = H;
        jobs[njobs].j = j; jobs[njobs].out = out; jobs[njobs].y0 = y0; jobs[njobs].y1 = y1; jobs[njobs].done = 0;
    }
    for (int i = 0; i < njobs - 1; i++) {
        char *stk = (char *)malloc(JPEG_CC_STACK);
        if (!stk || sys_clone((void *)jpeg_cc_thread, stk + JPEG_CC_STACK, &jobs[i]) < 0) {
            jpeg_cc_range(j, out, jobs[i].y0, jobs[i].y1);   /* OOM/spawn failure: just do it inline */
            jobs[i].done = 1;
        }
    }
    jpeg_cc_range(j, out, jobs[njobs - 1].y0, jobs[njobs - 1].y1);   /* this thread's own share */
    for (int i = 0; i < njobs - 1; i++)
        while (!jobs[i].done) { }   /* busy-wait join: each chunk is real work, not worth a syscall to block on */
}
#endif

int jpeg_decode(const uint8_t *data, int len, uint8_t *out, int out_cap,
                uint8_t *scratch, int scratch_cap, int *width, int *height) {
    jctx J; memset(&J, 0, sizeof(J));
    jctx *j = &J;
    j->d = data; j->len = len; j->p = 0; j->ri = 0;

    if (len < 2 || data[0] != 0xFF || data[1] != 0xD8) return JE_ERR;   /* SOI */
    j->p = 2;
    int have_sof = 0, coef_ready = 0;

    for (;;) {
        if (j->p + 2 > j->len) return JE_ERR;
        if (data[j->p] != 0xFF) { j->p++; continue; }
        int m = data[j->p + 1]; j->p += 2;
        if (m == 0xD9) { if (j->progressive) break; return JE_ERR; }   /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;  /* standalone */
        if (j->p + 2 > j->len) return JE_ERR;
        int seglen = rd16(j) - 2;
        if (seglen < 0 || j->p + seglen > j->len) return JE_ERR;
        int segend = j->p + seglen;
        int rc = JE_OK;
        switch (m) {
        case 0xDB: rc = parse_dqt(j, seglen); break;          /* DQT */
        case 0xC0: case 0xC1:                                 /* SOF0 baseline / SOF1 ext. seq. */
            if (have_sof) return JE_ERR;
            rc = parse_sof(j, seglen); have_sof = 1; j->progressive = 0; break;
        case 0xC2:                                            /* SOF2 progressive */
            if (have_sof) return JE_ERR;
            rc = parse_sof(j, seglen); have_sof = 1; j->progressive = 1; break;
        case 0xC4: rc = parse_dht(j, seglen); break;          /* DHT */
        case 0xDD:                                            /* DRI: body is a 2-byte restart interval */
            if (seglen < 2) return JE_ERR;                    /* guard rd16's 2-byte read: line 577 ensures p+seglen<=len, so seglen>=2 => p+2<=len (a short/truncated DRI would else OOB-read past data+len) */
            j->ri = rd16(j); break;
        case 0xDA: rc = parse_sos(j, seglen); break;          /* SOS */
        default: break;                                       /* APPn/COM/etc: skip */
        }
        if (rc != JE_OK) return JE_ERR;
        j->p = segend;
        if (m == 0xDA) {
            if (!j->progressive) break;                        /* baseline: one scan, below */
            if (!have_sof) return JE_ERR;                      /* progressive: decode this scan */
            if (!coef_ready) {
                if (setup_coef(j, scratch, scratch_cap) != JE_OK) return JE_ERR;
                coef_ready = 1;
            }
            if (decode_scan_prog(j) != JE_OK) return JE_ERR;
            /* skip remaining entropy bytes to the next real marker */
            while (j->p + 1 < j->len &&
                   !(j->d[j->p] == 0xFF && j->d[j->p+1] != 0x00 &&
                     !(j->d[j->p+1] >= 0xD0 && j->d[j->p+1] <= 0xD7)))
                j->p++;
        }
    }
    if (!have_sof) return JE_ERR;
    *width = j->W; *height = j->H;

    if (!j->progressive) {
        /* baseline: lay out planes, decode every MCU straight to samples */
        setup_dims(j);
        long need = 0;
        for (int i = 0; i < j->ncomp; i++) need += (long)j->comp[i].px * j->comp[i].py;
        if (need > scratch_cap) return JE_ERR;
        long off = 0;
        for (int i = 0; i < j->ncomp; i++) {
            j->comp[i].plane = scratch + off;
            off += (long)j->comp[i].px * j->comp[i].py;
            j->comp[i].dcpred = 0;
        }
        int restart = j->ri;
        for (int my = 0; my < j->mcuy; my++) {
            for (int mx = 0; mx < j->mcux; mx++) {
                for (int i = 0; i < j->ncomp; i++) {
                    comp_t *c = &j->comp[i];
                    for (int by = 0; by < c->vs; by++)
                        for (int bx = 0; bx < c->hs; bx++) {
                            int ox = (mx * c->hs + bx) * 8, oy = (my * c->vs + by) * 8;
                            uint8_t *blkout = c->plane + (long)oy * c->px + ox;
                            if (decode_block(j, c, blkout, c->px) != JE_OK) return JE_ERR;
                        }
                }
                if (j->ri && --restart == 0 && !(mx == j->mcux - 1 && my == j->mcuy - 1)) {
                    j->bitcnt = 0; j->eod = 0;
                    while (j->p + 1 < j->len && !(j->d[j->p] == 0xFF && j->d[j->p+1] >= 0xD0 && j->d[j->p+1] <= 0xD7))
                        j->p++;
                    if (j->p + 1 < j->len) j->p += 2;
                    for (int i = 0; i < j->ncomp; i++) j->comp[i].dcpred = 0;
                    restart = j->ri;
                }
            }
        }
    } else {
        /* progressive: coefficients are complete — dequantize + IDCT each block */
        if (!coef_ready) return JE_ERR;
        for (int i = 0; i < j->ncomp; i++) {
            comp_t *c = &j->comp[i];
            for (int byi = 0; byi < c->by; byi++)
                for (int bxi = 0; bxi < c->bx; bxi++) {
                    int16_t *cf = j->coef[i] + ((long)byi * c->bx + bxi) * 64;
                    long blk[64];
                    for (int k = 0; k < 64; k++) blk[k] = (long)cf[k] * j->qt[c->tq][k];
                    uint8_t *o = c->plane + (long)(byi * 8) * c->px + bxi * 8;
                    idct_block(blk, o, c->px);
                }
        }
    }

    /* upsample chroma + YCbCr->RGB into the RGBA output */
    if ((long)j->W * j->H * 4 > out_cap) return JE_ERR;
#ifdef JPEG_THREADED
    jpeg_color_convert(j, out);
#else
    jpeg_cc_range(j, out, 0, j->H);
#endif
    return JE_OK;
}
