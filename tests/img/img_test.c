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

static int wbe32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; return 4; }

/* Build a valid 8x8 PNG (8-bit, given colour type 0/2/4/6) whose IDAT is a
 * single STORED (uncompressed) deflate block — so inflate is effectively a
 * memcpy and the decode ALWAYS reaches recon_filters + expand_px on bytes we
 * control (CRCs are not checked by png.c). The scanlines (a filter byte + grey
 * pixels per row) are written plainly so the caller can fuzz them in place.
 * Returns total length; *scan_off = first scanline byte; *rawn = raw_need. */
static int build_png(uint8_t *g, int color, int *scan_off, int *rawn) {
    int bpp = (color==0)?1 : (color==2)?3 : (color==4)?2 : 4;
    int W=8, H=8, raw=(W*bpp+1)*H, p=0;
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    for (int i=0;i<8;i++) g[p++]=sig[i];
    p+=wbe32(g+p,13); g[p++]='I';g[p++]='H';g[p++]='D';g[p++]='R';   /* IHDR */
    p+=wbe32(g+p,W); p+=wbe32(g+p,H);
    g[p++]=8; g[p++]=(uint8_t)color; g[p++]=0; g[p++]=0; g[p++]=0;   /* depth, colour, comp, filter, interlace */
    p+=wbe32(g+p,0);                                                 /* CRC (ignored) */
    int idat = 2 + 5 + raw;
    p+=wbe32(g+p,idat); g[p++]='I';g[p++]='D';g[p++]='A';g[p++]='T'; /* IDAT */
    g[p++]=0x78; g[p++]=0x01;                                        /* zlib header */
    g[p++]=0x01;                                                     /* BFINAL=1, BTYPE=0 (stored) */
    g[p++]=raw&0xFF; g[p++]=(raw>>8)&0xFF;                           /* LEN */
    g[p++]=(~raw)&0xFF; g[p++]=((~raw)>>8)&0xFF;                     /* NLEN = ~LEN */
    *scan_off = p;
    for (int y=0;y<H;y++) { g[p++]=0; for (int x=0;x<W*bpp;x++) g[p++]=0x80; }  /* filter None + grey */
    p+=wbe32(g+p,0);                                                 /* IDAT CRC (ignored) */
    p+=wbe32(g+p,0); g[p++]='I';g[p++]='E';g[p++]='N';g[p++]='D'; p+=wbe32(g+p,0);  /* IEND */
    *rawn = raw;
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

    /* 8. JPEG Huffman/IDCT path. Like GIF LZW, the magic-only prefix fuzz can't
     *    reach decode_block — it needs valid DQT+SOF0+DHT+SOS first — so the
     *    JPEG decoder's most complex untrusted-input code (Huffman decode, the
     *    run/zigzag expansion, dequant, IDCT, YCbCr->RGB) was unfuzzed. Embed a
     *    real baseline JPEG, decode it (proves the path), then fuzz the
     *    entropy-coded scan with output/scratch buffers sized to the exact image
     *    so ASan catches any block/plane/output over-write. */
    /* A real baseline grayscale 8x8 JPEG (PIL q80): SOI, JFIF APP0, DQT, SOF0,
     * DC+AC DHT, SOS, then the entropy-coded scan. Decodes to a gray gradient. */
    static const uint8_t jpg8[] = {
        0xff,0xd8,0xff,0xe0,0x00,0x10,0x4a,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,
        0x00,0x01,0x00,0x00,0xff,0xdb,0x00,0x43,0x00,0x06,0x04,0x05,0x06,0x05,0x04,0x06,
        0x06,0x05,0x06,0x07,0x07,0x06,0x08,0x0a,0x10,0x0a,0x0a,0x09,0x09,0x0a,0x14,0x0e,
        0x0f,0x0c,0x10,0x17,0x14,0x18,0x18,0x17,0x14,0x16,0x16,0x1a,0x1d,0x25,0x1f,0x1a,
        0x1b,0x23,0x1c,0x16,0x16,0x20,0x2c,0x20,0x23,0x26,0x27,0x29,0x2a,0x29,0x19,0x1f,
        0x2d,0x30,0x2d,0x28,0x30,0x25,0x28,0x29,0x28,0xff,0xc0,0x00,0x0b,0x08,0x00,0x08,
        0x00,0x08,0x01,0x01,0x11,0x00,0xff,0xc4,0x00,0x1f,0x00,0x00,0x01,0x05,0x01,0x01,
        0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,
        0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0xff,0xc4,0x00,0xb5,0x10,0x00,0x02,0x01,0x03,
        0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,0x01,0x7d,0x01,0x02,0x03,0x00,
        0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,
        0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,
        0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,
        0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,
        0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,
        0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,
        0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,
        0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,
        0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,
        0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,0xff,0xda,
        0x00,0x08,0x01,0x01,0x00,0x00,0x3f,0x00,0xe1,0x3e,0x1b,0x68,0x5f,0xea,0xbe,0x4f,
        0x4a,0xff,0xd9,
    };
    const int JPG_SCAN = 328, JLEN = (int)sizeof jpg8;   /* entropy data starts at 328 */
    {
        /* (a) decode the real JPEG — proves the harness reaches decode_block. */
        int w = 0, h = 0; long need = 0;
        if (jpeg_probe(jpg8, JLEN, &w, &h, &need) != 0 || w != 8 || h != 8 || need != 64) {
            printf("FAIL: jpeg_probe embedded 8x8 (w=%d h=%d need=%ld)\n", w, h, need); return 1; }
        int r = jpeg_decode(jpg8, JLEN, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        if (r != 0 || w != 8 || h != 8 || obuf[0] != 0 || obuf[3] != 255) {
            printf("FAIL: jpeg_decode embedded 8x8 (r=%d w=%d h=%d px0=%d a=%d)\n", r, w, h, obuf[0], obuf[3]); return 1; }

        /* (b) fuzz the entropy scan behind the valid header, into buffers sized
         *     EXACTLY to 8x8 (256 RGBA / 64 sample bytes). Point-mutate the scan
         *     (stays mostly valid -> decode runs deep) and sometimes truncate it
         *     (stresses bit-exhaustion mid-block). Dims never change, so the
         *     tight buffers stay correct and any over-write trips ASan. */
        uint8_t *jout = malloc(8*8*4), *jscr = malloc(64);
        uint8_t jf[512];
        for (int i = 0; i < ITERS && jout && jscr; i++) {
            for (int k = 0; k < JLEN; k++) jf[k] = jpg8[k];
            int K = 1 + (int)(xr() % 6);
            for (int k = 0; k < K; k++)
                jf[JPG_SCAN + (int)(xr() % (JLEN - JPG_SCAN - 2))] = (uint8_t)xr();
            int use = (xr() & 1) ? JLEN : JPG_SCAN + (int)(xr() % (JLEN - JPG_SCAN));
            int ww, hh;
            jpeg_decode(jf, use, jout, 8*8*4, jscr, 64, &ww, &hh);
        }
        free(jout); free(jscr);
    }

    /* 9. PNG recon-filter / pixel-expand path. inflate is fuzzed directly
     *    (section 4), but the PNG-specific recon_filters (the 5 filter types)
     *    and expand_px (per colour type) run only after a valid IHDR + zlib
     *    IDAT, which random bytes don't produce. build_png() wraps the
     *    scanlines in a STORED deflate block (inflate becomes a memcpy), so the
     *    decode always reaches recon+expand on bytes we control. The output
     *    buffer is sized to exactly 8x8 RGBA (256B) so an expand_px / output
     *    over-write trips an ASan redzone. */
    {
        static uint8_t pg[2048];
        static const int colors[4] = {0, 2, 4, 6};   /* grey, RGB, grey+A, RGBA */

        /* (a) every colour type decodes through recon+expand to grey pixels. */
        for (int ci = 0; ci < 4; ci++) {
            int so, rn, w, h;
            int len = build_png(pg, colors[ci], &so, &rn);
            int r = png_decode(pg, len, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
            if (r != 0 || w != 8 || h != 8 || obuf[0] != 0x80) {
                printf("FAIL: PNG colour %d decode (r=%d w=%d h=%d px0=%d)\n",
                       colors[ci], r, w, h, obuf[0]);
                return 1;
            }
        }

        /* (b) fuzz the scanlines (filter byte + pixels) of a random colour type,
         *     into a 256B output sized exactly to 8x8 RGBA. Mutated filter bytes
         *     exercise recon cases 0..4 + invalid (graceful -1); pixel bytes
         *     drive expand_px; the tight output catches any over-write. */
        uint8_t *pout = malloc(8*8*4);
        for (int i = 0; i < ITERS && pout; i++) {
            int so, rn, w, h;
            int len = build_png(pg, colors[xr() & 3], &so, &rn);
            int K = 1 + (int)(xr() % 8);
            for (int k = 0; k < K; k++) pg[so + (int)(xr() % rn)] = (uint8_t)xr();
            png_decode(pg, len, pout, 8*8*4, sbuf, sizeof sbuf, &w, &h);
        }
        free(pout);
    }

    printf("imgtest: M422 DRI PoC + truncated headers + BMP 2x2 + GIF 2x2 LZW + JPEG 8x8 + PNG colours + gzip round-trip + %d decoder + %d DEFLATE + %d BMP + %d gzip + %d GIF-LZW + %d JPEG-scan + %d PNG-scan fuzz iters — ASan/UBSan clean\n", ITERS, ITERS, ITERS, ITERS, ITERS, ITERS, ITERS);
    return 0;
}
