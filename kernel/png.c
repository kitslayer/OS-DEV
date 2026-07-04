/*
 * png.c — a minimal PNG decoder built on the DEFLATE inflater (inflate.c).
 *
 * Supports the common case: 8-bit depth, non-interlaced, colour types 0
 * (grayscale), 2 (truecolour RGB), 4 (gray+alpha) and 6 (RGBA). It reads IHDR,
 * concatenates the IDAT data, inflates it (skipping the 2-byte zlib header),
 * reverses the per-scanline filters (None/Sub/Up/Average/Paeth, RFC 2083), and
 * expands every pixel to RGBA. Buffers are caller-provided so it's standalone
 * and host-testable. CRCs are not checked (we trust the source).
 */
#include "png.h"
#include "inflate.h"

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int iabs(int x) { return x < 0 ? -x : x; }
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = iabs(p - a), pb = iabs(p - b), pc = iabs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Reverse the per-scanline filters for `rows` scanlines, each laid out as
 * [filter byte][rowbytes] contiguously from `base`. In-place. 0 / -1. */
static int recon_filters(uint8_t *base, int rows, long rowbytes, int bpp) {
    for (int y = 0; y < rows; y++) {
        uint8_t *row  = base + (long)y * (rowbytes + 1);
        int filt = row[0];
        uint8_t *cur  = row + 1;
        uint8_t *prev = (y > 0) ? base + (long)(y - 1) * (rowbytes + 1) + 1 : 0;
        for (long x = 0; x < rowbytes; x++) {
            int a = (x >= bpp) ? cur[x - bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = cur[x];
            switch (filt) {
                case 0: break;
                case 1: v = (v + a) & 0xFF; break;
                case 2: v = (v + b) & 0xFF; break;
                case 3: v = (v + ((a + b) >> 1)) & 0xFF; break;
                case 4: v = (v + paeth(a, b, c)) & 0xFF; break;
                default: return -1;
            }
            cur[x] = (uint8_t)v;
        }
    }
    return 0;
}

/* Expand one reconstructed pixel (`px`, bpp bytes) to RGBA at `o`. */
static void expand_px(int color, const uint8_t *px, const uint8_t *pal, int pal_n,
                      const uint8_t *trns, int trns_n, uint8_t *o) {
    uint8_t r, g, bl, al;
    if (color == 3)      { int idx = px[0] < pal_n ? px[0] : 0;
                           r = pal[idx*3]; g = pal[idx*3+1]; bl = pal[idx*3+2];
                           al = idx < trns_n ? trns[idx] : 255; }
    else if (color == 0) { r = g = bl = px[0];
                           al = (trns_n >= 2 && px[0] == trns[1]) ? 0 : 255; }
    else if (color == 2) { r = px[0]; g = px[1]; bl = px[2];
                           al = (trns_n >= 6 && px[0]==trns[1] && px[1]==trns[3] && px[2]==trns[5]) ? 0 : 255; }
    else if (color == 4) { r = g = bl = px[0]; al = px[1]; }
    else                 { r = px[0]; g = px[1]; bl = px[2]; al = px[3]; }
    o[0] = r; o[1] = g; o[2] = bl; o[3] = al;
}

/* Adam7 interlace passes: x/y origin and x/y step for each of the 7 passes. */
static const int A7x[7]  = {0,4,0,2,0,1,0};
static const int A7y[7]  = {0,0,4,0,2,0,1};
static const int A7dx[7] = {8,8,4,4,2,2,1};
static const int A7dy[7] = {8,8,8,4,4,2,2};

/* Parallel per-pixel RGBA expansion (M1533-follow-on, same pattern as jpeg.c's
 * jpeg_color_convert): unlike recon_filters() above (Up/Average/Paeth read the
 * PREVIOUS row's reconstructed bytes, so filter reversal must stay strictly
 * sequential), expand_px() only reads the now-fully-reconstructed `scratch`
 * rows plus the constant palette/trns tables, and writes a disjoint slice of
 * `out` — the exact same "independent per pixel" shape jpeg.c's colour
 * conversion has, once the (fast, O(n)) sequential unfilter pass is done.
 * Scoped to the non-interlaced path only: Adam7 (7 differently-sized passes,
 * each with its own scatter geometry) is a rarer, structurally different
 * loop, not a same-shape extension of this one — left untouched. Guarded
 * behind PNG_THREADED for the same host-testability reason as jpeg.c. */
struct png_expand_job_shape {
    int color; const uint8_t *scratch; long stride; int bpp;
    const uint8_t *pal; int pal_n; const uint8_t *trns; int trns_n;
    int width; uint8_t *out;
};
static void png_expand_range(const struct png_expand_job_shape *s, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        const uint8_t *cur = s->scratch + (long)y * (s->stride + 1) + 1;
        uint8_t *o = s->out + (long)y * s->width * 4;
        for (int x = 0; x < s->width; x++)
            expand_px(s->color, cur + (long)x * s->bpp, s->pal, s->pal_n, s->trns, s->trns_n, o + x * 4);
    }
}

#ifdef PNG_THREADED
#include "ulib.h"

#define PNG_MAX_THREADS     4
#define PNG_EXPAND_MIN_PIXELS (200 * 1000)   /* see jpeg.c's JPEG_CC_MIN_PIXELS for why pixels, not rows */
#define PNG_EXPAND_STACK    (64 * 1024)

struct png_expand_job { const struct png_expand_job_shape *s; int y0, y1; volatile int done; };
static void png_expand_thread(void *arg) {
    struct png_expand_job *job = (struct png_expand_job *)arg;
    png_expand_range(job->s, job->y0, job->y1);
    job->done = 1;              /* LAST write before exiting -- the join loop waits on this */
    sys_thread_exit();
}
static void png_expand_parallel(const struct png_expand_job_shape *s, int height) {
    if ((long)s->width * height < PNG_EXPAND_MIN_PIXELS) { png_expand_range(s, 0, height); return; }

    struct png_expand_job jobs[PNG_MAX_THREADS];
    int njobs = 0;
    int per = (height + PNG_MAX_THREADS - 1) / PNG_MAX_THREADS;
    for (int y0 = 0; y0 < height && njobs < PNG_MAX_THREADS; y0 += per, njobs++) {
        int y1 = y0 + per; if (y1 > height) y1 = height;
        jobs[njobs].s = s; jobs[njobs].y0 = y0; jobs[njobs].y1 = y1; jobs[njobs].done = 0;
    }
    for (int i = 0; i < njobs - 1; i++) {
        char *stk = (char *)malloc(PNG_EXPAND_STACK);
        if (!stk || sys_clone((void *)png_expand_thread, stk + PNG_EXPAND_STACK, &jobs[i]) < 0) {
            png_expand_range(jobs[i].s, jobs[i].y0, jobs[i].y1);   /* OOM/spawn failure: just do it inline */
            jobs[i].done = 1;
        }
    }
    png_expand_range(s, jobs[njobs - 1].y0, jobs[njobs - 1].y1);   /* this thread's own share */
    for (int i = 0; i < njobs - 1; i++)
        while (!jobs[i].done) { }   /* busy-wait join: each chunk is real work, not worth a syscall to block on */
}
#endif

int png_decode(const uint8_t *data, int len, uint8_t *out, int out_cap,
               uint8_t *scratch, int scratch_cap, int *w, int *h) {
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if (len < 8 + 25) return -1;
    for (int i = 0; i < 8; i++) if (data[i] != sig[i]) return -1;

    int width = 0, height = 0, bitdepth = 0, color = 0, interlace = 0;
    int have_ihdr = 0, idat_len = 0;
    uint8_t pal[256 * 3]; int pal_n = 0;                 /* PLTE palette (colour type 3) */
    uint8_t trns[256]; int trns_n = 0;                   /* tRNS per-index alpha (palette) */

    /* First pass: read IHDR and total the IDAT payload size. */
    int p = 8;
    while (p + 8 <= len) {
        uint32_t clen = be32(data + p);
        const uint8_t *type = data + p + 4, *body = data + p + 8;
        if ((uint64_t)p + 12 + clen > (uint64_t)len) return -1;   /* chunk overruns file */
        if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R') {
            if (clen < 13) return -1;
            width = (int)be32(body); height = (int)be32(body + 4);
            bitdepth = body[8]; color = body[9]; interlace = body[12];
            have_ihdr = 1;
            /* bound dimensions: keeps (stride+1)*height and width*height*4 well
             * within 64-bit `long`, so the buffer-cap checks below can't be
             * defeated by integer overflow (a crafted IHDR could otherwise force
             * an out-of-bounds write). The caller clamps tighter still. */
            if (width <= 0 || height <= 0 || width > 32768 || height > 32768) return -1;
            if (bitdepth != 8 || interlace > 1) return -1;   /* 0 = none, 1 = Adam7 */
            if (color != 0 && color != 2 && color != 3 && color != 4 && color != 6) return -1;
        } else if (type[0]=='P'&&type[1]=='L'&&type[2]=='T'&&type[3]=='E') {
            pal_n = (int)clen / 3; if (pal_n > 256) pal_n = 256;     /* RGB triples */
            for (int i = 0; i < pal_n * 3; i++) pal[i] = body[i];
        } else if (type[0]=='t'&&type[1]=='R'&&type[2]=='N'&&type[3]=='S') {
            trns_n = (int)clen; if (trns_n > 256) trns_n = 256;      /* per-index alpha */
            for (int i = 0; i < trns_n; i++) trns[i] = body[i];
        } else if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
            idat_len += (int)clen;
        } else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') {
            break;
        }
        p += 12 + (int)clen;       /* length + type + body + CRC */
    }
    if (!have_ihdr || idat_len < 2) return -1;
    if (idat_len > scratch_cap) return -1;

    /* Second pass: copy the IDAT bytes contiguously (in file order) into the
     * tail of scratch — the zlib stream the inflater reads from. */
    uint8_t *zs = scratch + scratch_cap - idat_len;
    {
        int q = 8, wpos = 0;
        while (q + 8 <= len) {
            uint32_t clen = be32(data + q);
            const uint8_t *type = data + q + 4, *body = data + q + 8;
            if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T')
                for (uint32_t i = 0; i < clen; i++) zs[wpos++] = body[i];
            if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') break;
            q += 12 + (int)clen;
        }
    }

    if (color == 3 && pal_n == 0) return -1;               /* palette image without a PLTE */
    int bpp = (color == 0 || color == 3) ? 1 : (color == 2) ? 3 : (color == 4) ? 2 : 4;  /* bytes/pixel */
    if ((long)width * height * 4 > out_cap) return -1;

    /* total inflated size: a filter byte per scanline. For Adam7 that's summed
     * over the 7 reduced-resolution passes; otherwise one image of `height` rows. */
    long raw_need;
    int pw[7] = {0}, ph[7] = {0};
    if (interlace) {
        raw_need = 0;
        for (int i = 0; i < 7; i++) {
            pw[i] = (width  > A7x[i]) ? (width  - A7x[i] + A7dx[i] - 1) / A7dx[i] : 0;
            ph[i] = (height > A7y[i]) ? (height - A7y[i] + A7dy[i] - 1) / A7dy[i] : 0;
            if (pw[i] > 0 && ph[i] > 0) raw_need += ((long)pw[i] * bpp + 1) * ph[i];
        }
    } else {
        raw_need = ((long)width * bpp + 1) * height;
    }
    /* inflate output goes to the front of scratch; the zlib stream sits in the
     * tail (zs). They must not overlap. */
    if (raw_need > scratch_cap - idat_len) return -1;

    int n = inflate(zs + 2, idat_len - 2, scratch, (int)raw_need);  /* +2: skip zlib hdr */
    if (n != (int)raw_need) return -1;

    if (!interlace) {
        long stride = (long)width * bpp;
        if (recon_filters(scratch, height, stride, bpp) != 0) return -1;
        struct png_expand_job_shape shp = { color, scratch, stride, bpp, pal, pal_n, trns, trns_n, width, out };
#ifdef PNG_THREADED
        png_expand_parallel(&shp, height);
#else
        png_expand_range(&shp, 0, height);
#endif
    } else {
        /* Adam7: reconstruct each pass independently, then scatter its pixels to
         * their grid positions in the full image. */
        uint8_t *base = scratch;
        for (int i = 0; i < 7; i++) {
            if (pw[i] <= 0 || ph[i] <= 0) continue;
            long rowbytes = (long)pw[i] * bpp;
            if (recon_filters(base, ph[i], rowbytes, bpp) != 0) return -1;
            for (int row = 0; row < ph[i]; row++) {
                const uint8_t *cur = base + (long)row * (rowbytes + 1) + 1;
                int fy = A7y[i] + row * A7dy[i];
                for (int col = 0; col < pw[i]; col++) {
                    int fx = A7x[i] + col * A7dx[i];
                    expand_px(color, cur + (long)col * bpp, pal, pal_n, trns, trns_n,
                              out + ((long)fy * width + fx) * 4);
                }
            }
            base += (rowbytes + 1) * ph[i];
        }
    }
    *w = width; *h = height;
    return 0;
}
