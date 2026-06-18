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
#include <stdlib.h>

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

/* Build a valid GIF89a up to (but not including) the LZW data: a 4-colour
 * global colour table (colour 0 = 0x10,0x20,0x30), one image covering the whole
 * logical screen, no local table, not interlaced. Returns the byte count; the
 * caller appends the LZW min-code-size byte + sub-blocks. Lets the GIF fuzz
 * reach lzw_decode, which the magic-only prefix fuzz cannot (it needs a
 * structurally valid header first). */
static int build_gif(uint8_t *g, int iw, int ih) {
    int p = 0;
    g[p++]='G'; g[p++]='I'; g[p++]='F'; g[p++]='8'; g[p++]='9'; g[p++]='a';
    g[p++]=iw&0xFF; g[p++]=(iw>>8)&0xFF;                 /* logical screen w */
    g[p++]=ih&0xFF; g[p++]=(ih>>8)&0xFF;                 /* logical screen h */
    g[p++]=0x80|0x01; g[p++]=0; g[p++]=0;                /* GCT present, 4 colours */
    g[p++]=0x10; g[p++]=0x20; g[p++]=0x30;               /* colour 0 */
    for (int i = 0; i < 9; i++) g[p++]=0;                /* colours 1..3 */
    g[p++]=0x2C; g[p++]=0; g[p++]=0; g[p++]=0; g[p++]=0; /* image separator + left/top */
    g[p++]=iw&0xFF; g[p++]=(iw>>8)&0xFF;                 /* image w */
    g[p++]=ih&0xFF; g[p++]=(ih>>8)&0xFF;                 /* image h */
    g[p++]=0;                                            /* no local table, no interlace */
    return p;
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

    /* 7. GIF LZW path. The magic-only prefix fuzz (section 3) can't reach
     *    lzw_decode — it needs a structurally valid header first — so the GIF
     *    decoder's trickiest untrusted-input code (variable-width codes, the
     *    dictionary, KwKwK, the sub-block chain) was effectively unfuzzed.
     *    build_gif() gives a real header; here we (a) decode a known 2x2 to
     *    prove the harness reaches LZW, (b) reject oversized dimensions, and
     *    (c) fuzz the compressed sub-block stream under ASan/UBSan. */
    {
        static uint8_t g[300];
        int w2, h2;

        /* (a) known-good 2x2, all palette index 0 -> colour (0x10,0x20,0x30).
         *     LZW (min code size 2): CLEAR, 0,0,0,0, END packs to 04 00 05. */
        int p = build_gif(g, 2, 2);
        g[p++]=2;                                        /* LZW min code size */
        g[p++]=3; g[p++]=0x04; g[p++]=0x00; g[p++]=0x05; /* one 3-byte sub-block */
        g[p++]=0;                                        /* block terminator */
        int r = gif_decode(g, p, obuf, sizeof obuf, sbuf, sizeof sbuf, &w2, &h2);
        if (!(r == 0 && w2 == 2 && h2 == 2 &&
              obuf[0]==0x10 && obuf[1]==0x20 && obuf[2]==0x30 && obuf[3]==255 &&
              obuf[12]==0x10 && obuf[13]==0x20 && obuf[14]==0x30 && obuf[15]==255)) {
            printf("FAIL: GIF 2x2 LZW decode wrong (r=%d w=%d h=%d px0=%d,%d,%d)\n",
                   r, w2, h2, obuf[0], obuf[1], obuf[2]);
            return 1;
        }

        /* (b) valid header, oversized dimensions -> graceful reject (the
         *     decompression-bomb / dimension-overflow guard), never an overflow. */
        p = build_gif(g, 5000, 5000);
        if (gif_decode(g, p, obuf, sizeof obuf, sbuf, sizeof sbuf, &w2, &h2) != -1) {
            printf("FAIL: GIF 5000x5000 dimensions not rejected\n"); return 1;
        }

        /* (c) fuzz the LZW sub-block stream behind a valid header. Two things
         *     make this actually exercise the bounds: (1) SMALL randomised dims
         *     (idx_cap 1..64) so the per-pixel output bound (out < idx_cap) is
         *     hit constantly while the dictionary / prefix-walk / KwKwK /
         *     code_size logic still runs; (2) scratch + output buffers malloc'd
         *     to the EXACT image size — the kernel sizes scratch to iw*ih, so a
         *     4MB static buffer would mask a write past the cap; tight malloc'd
         *     buffers give ASan redzones right at the boundary. */
        for (int i = 0; i < ITERS; i++) {
            int iw = 1 + (int)(xr() % 8), ih = 1 + (int)(xr() % 8);
            p = build_gif(g, iw, ih);
            g[p++] = (uint8_t)(2 + (xr() % 7));          /* min code size 2..8 (valid) */
            int payload = (int)(xr() % (sizeof g - (unsigned)p));
            for (int j = 0; j < payload; j++) g[p++] = (uint8_t)xr();
            uint8_t *tscr = malloc((size_t)iw * ih);     /* indices: exactly iw*ih */
            uint8_t *tout = malloc((size_t)iw * ih * 4); /* RGBA: exactly iw*ih*4 */
            if (tscr && tout) gif_decode(g, p, tout, iw*ih*4, tscr, iw*ih, &w2, &h2);
            free(tscr); free(tout);
        }
    }

    printf("imgtest: M422 DRI PoC + truncated headers + BMP 2x2 + GIF 2x2 LZW + gzip round-trip + %d decoder + %d DEFLATE + %d BMP + %d gzip + %d GIF-LZW fuzz iters — ASan/UBSan clean\n", ITERS, ITERS, ITERS, ITERS, ITERS);
    return 0;
}
