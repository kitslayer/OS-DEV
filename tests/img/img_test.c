/*
 * Image-decoder regression + fuzz test (host-side, ASan/UBSan).
 *
 * The kernel-side image decoders (jpeg/png/gif/inflate) parse UNTRUSTED bytes
 * fetched from the web, with no stack guard page, so an OOB is kernel
 * corruption. This locks the M422 fix (a JPEG DRI marker read 2 bytes past the
 * input on a short segment) and fuzzes all three decoders against adversarial
 * input. Run via tests/run-img-tests.sh ("make imgtest"). A clean exit = pass;
 * any OOB/overflow aborts under ASan/UBSan; a non-terminating decoder hangs.
 */
#include <stdint.h>
#include <stdio.h>

int jpeg_probe (const uint8_t *, int, int *, int *, long *);
int jpeg_decode(const uint8_t *, int, uint8_t *, int, uint8_t *, int, int *, int *);
int png_decode (const uint8_t *, int, uint8_t *, int, uint8_t *, int, int *, int *);
int gif_decode (const uint8_t *, int, uint8_t *, int, uint8_t *, int, int *, int *);
int bmp_decode (const uint8_t *, int, uint8_t *, int, int *, int *);
int inflate    (const uint8_t *, int, uint8_t *, int);   /* raw DEFLATE */
int gz_inflate (const uint8_t *, int, uint8_t *, int);   /* gzip wrapper */

static uint8_t obuf[4u << 20], sbuf[4u << 20];   /* 4 MB each (BSS) */

static uint32_t rs = 0xC0FFEEu;                  /* deterministic xorshift32 */
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void run_all(const uint8_t *d, int n) {
    int w, h; long need;
    jpeg_probe (d, n, &w, &h, &need);
    jpeg_decode(d, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    png_decode (d, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    gif_decode (d, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    bmp_decode (d, n, obuf, sizeof obuf, &w, &h);
}

int main(void) {
    /* 1. M422 regression: a DRI (0xDD) claiming segment length 2 or 3 (body 0/1
     *    bytes) at EOF must NOT make rd16 read past data+len. Without the
     *    seglen>=2 guard, ASan flags an OOB read here. */
    static const uint8_t dri_poc1[] = { 0xFF,0xD8, 0xFF,0xDD, 0x00,0x02 };
    static const uint8_t dri_poc2[] = { 0xFF,0xD8, 0xFF,0xDD, 0x00,0x03, 0x00 };
    run_all(dri_poc1, sizeof dri_poc1);
    run_all(dri_poc2, sizeof dri_poc2);

    /* 2. Truncated / bare-magic headers (must error cleanly, not over-read). */
    static const uint8_t t_jpeg[] = { 0xFF,0xD8 };
    static const uint8_t t_png [] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    static const uint8_t t_gif [] = { 0x47,0x49,0x46,0x38,0x39,0x61 };
    run_all(t_jpeg, sizeof t_jpeg);
    run_all(t_png,  sizeof t_png);
    run_all(t_gif,  sizeof t_gif);

    /* 3. Deterministic fuzz: random bytes, often prefixed with a real magic so
     *    the probe lets the decode body run, through all three decoders. */
    static const uint8_t mj[] = { 0xFF,0xD8 };
    static const uint8_t mp[] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    static const uint8_t mg[] = { 0x47,0x49,0x46,0x38,0x39,0x61 };
    uint8_t f[96];
    const int ITERS = 120000;
    for (int i = 0; i < ITERS; i++) {
        int n = 2 + (int)(xr() % 90);            /* 2..91 bytes */
        int pre = 0;
        switch (i & 3) {
            case 0: for (; pre < (int)sizeof mj && pre < n; pre++) f[pre] = mj[pre]; break;
            case 1: for (; pre < (int)sizeof mp && pre < n; pre++) f[pre] = mp[pre]; break;
            case 2: for (; pre < (int)sizeof mg && pre < n; pre++) f[pre] = mg[pre]; break;
            default: break;                       /* pure random */
        }
        for (int j = pre; j < n; j++) f[j] = (uint8_t)xr();
        run_all(f, n);
    }

    /* 3b. gzip round-trip: a real gzip stream (python: gzip.compress("Hello from
     *     a gzip file! ...")) must decompress to the exact original bytes. */
    {
        static const uint8_t gz[] = {
            0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0xff,0xf3,0x48,0xcd,0xc9,0xc9,0x57,
            0x48,0x2b,0xca,0xcf,0x55,0x48,0x54,0x48,0xaf,0xca,0x2c,0x50,0x48,0xcb,0xcc,0x49,
            0x55,0x54,0xf0,0x0f,0xd6,0x75,0x71,0x0d,0x53,0x48,0x4e,0xcc,0x53,0xc8,0xcb,0x2f,
            0x57,0x48,0x49,0x4d,0xce,0xcf,0x2d,0x28,0x4a,0x2d,0x2e,0x56,0xd0,0x4b,0xaf,0x52,
            0x28,0xcf,0x2c,0xc9,0x50,0xc8,0x2c,0x29,0x56,0xc8,0x2f,0xcf,0x53,0x70,0x71,0x75,
            0xf3,0x71,0x0c,0x71,0x05,0xab,0x49,0x49,0x2d,0xd2,0xe3,0x02,0x00,0x46,0x8a,0x1f,
            0x3f,0x54,0x00,0x00,0x00 };
        static const char exp[] = "Hello from a gzip file! OS-DEV can now decompress .gz with its own DEFLATE decoder.\n";
        int n = gz_inflate(gz, (int)sizeof gz, obuf, sizeof obuf);
        int explen = (int)(sizeof exp - 1);
        if (n != explen) { printf("FAIL: gz_inflate length %d != %d\n", n, explen); return 1; }
        for (int i = 0; i < explen; i++) if (obuf[i] != (uint8_t)exp[i]) { printf("FAIL: gz_inflate byte %d\n", i); return 1; }
    }

    /* 4. Direct DEFLATE fuzz: random streams straight into inflate(). The PNG
     *    path reaches inflate only shallowly (PNG header parsing rejects most
     *    random bytes first), so this directly exercises the huffman-table and
     *    LZ77 back-reference paths — the classic DEFLATE OOB/loop vectors. */
    for (int i = 0; i < ITERS; i++) {
        int n = 1 + (int)(xr() % 80);
        for (int j = 0; j < n; j++) f[j] = (uint8_t)xr();
        inflate(f, n, obuf, sizeof obuf);
    }

    /* 4b. gzip-header fuzz: a valid magic (1f 8b 08) + random flags/fields/body,
     *     so the header-skip (FEXTRA/FNAME/FCOMMENT/FHCRC) runs against truncated
     *     and adversarial input — the gz_inflate over-read vectors. */
    for (int i = 0; i < ITERS; i++) {
        int n = 3 + (int)(xr() % 80);
        for (int j = 0; j < n; j++) f[j] = (uint8_t)xr();
        f[0] = 0x1f; f[1] = 0x8b; f[2] = 0x08;
        gz_inflate(f, n, obuf, sizeof obuf);
    }

    /* 5. BMP correctness: a 2x2 24-bit bottom-up BMP with four known colours
     *    must decode to the right top-down RGBA pixels (and prove the bottom-up
     *    row flip + BGR->RGBA order). */
    static const uint8_t bmp24[70] = {
        'B','M', 70,0,0,0, 0,0,0,0, 54,0,0,0,            /* file: size=70, pixels@54 */
        40,0,0,0, 2,0,0,0, 2,0,0,0, 1,0, 24,0, 0,0,0,0,  /* DIB: 40, w=2,h=2, 1 plane, 24bpp, BI_RGB */
        16,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,    /* imgsize=16, ppm/clrused/clrimp = 0 */
        255,0,0,  255,255,255, 0,0,                       /* bottom row (y=1): blue,  white  + pad */
        0,0,255,  0,255,0,     0,0,                       /* top row    (y=0): red,   green  + pad */
    };
    {
        int w = 0, hh = 0;
        int r = bmp_decode(bmp24, sizeof bmp24, obuf, sizeof obuf, &w, &hh);
        const uint8_t exp[16] = { 255,0,0,255,  0,255,0,255,  0,0,255,255,  255,255,255,255 };
        int ok = (r == 0 && w == 2 && hh == 2);
        for (int i = 0; ok && i < 16; i++) if (obuf[i] != exp[i]) ok = 0;
        if (!ok) { printf("FAIL: BMP 2x2 decode wrong (r=%d w=%d h=%d)\n", r, w, hh); return 1; }
    }

    /* 6. BMP-targeted fuzz: 'BM' + a 40-size DIB header, then random bytes, so
     *    the parse runs past the magic into the stride / data-offset / palette
     *    math (the BMP OOB vectors) with random width/height/bpp/offset. */
    for (int i = 0; i < ITERS; i++) {
        int n = 54 + (int)(xr() % 40);           /* 54..93 bytes */
        for (int j = 0; j < n; j++) f[j] = (uint8_t)xr();
        f[0] = 'B'; f[1] = 'M';
        f[14] = 40; f[15] = f[16] = f[17] = 0;   /* DIB size = 40 -> parser proceeds */
        int w, h; bmp_decode(f, n, obuf, sizeof obuf, &w, &h);
    }

    printf("imgtest: M422 DRI PoC + truncated headers + BMP 2x2 + gzip round-trip + %d decoder + %d DEFLATE + %d BMP + %d gzip fuzz iters — ASan/UBSan clean\n", ITERS, ITERS, ITERS, ITERS);
    return 0;
}
