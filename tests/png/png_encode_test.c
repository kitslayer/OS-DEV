/*
 * PNG encoder round-trip + bounds test (host-side, ASan/UBSan).
 *
 * png_encode (kernel/png_encode.c) builds a complete 8-bit truecolour PNG from
 * an RGB buffer, reusing raw_deflate for the IDAT zlib stream. This test proves
 * the encoder is *correct* by round-tripping: for a range of images (solid
 * colour, gradients, deterministic noise, tiny 1x1/2x2/odd sizes and a larger
 * 256x256), it encodes, then decodes the result with the kernel's own
 * png_decode and asserts every pixel's R,G,B survived exactly and alpha == 255,
 * with the right width/height. It also checks png_encode returns -1 (rather than
 * overflowing) when outcap or scratchcap is one byte too small. It runs under
 * ASan+UBSan, so any out-of-bounds or UB also aborts.
 *
 * Build/run:
 *   gcc -std=gnu11 -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *       -Ikernel/include tests/png/png_encode_test.c \
 *       kernel/png_encode.c kernel/png.c kernel/inflate.c kernel/deflate.c \
 *       -o /tmp/pngenc && /tmp/pngenc
 *
 * A clean exit with the PASS line = success.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int png_encode(const uint8_t *, int, int, uint8_t *, int, uint8_t *, int);
int png_decode(const uint8_t *data, int len, uint8_t *out, int out_cap,
               uint8_t *scratch, int scratch_cap, int *w, int *h);

/* Generous BSS buffers. 256x256 RGB = 192 KB; filtered ~193 KB; PNG output for
 * incompressible noise can exceed the raw size a touch, so give each megabytes. */
static uint8_t rgb_buf[4u << 20];     /* source RGB                 */
static uint8_t png_buf[8u << 20];     /* encoded PNG                */
static uint8_t enc_scr[8u << 20];     /* scratch for png_encode     */
static uint8_t dec_rgba[8u << 20];    /* decoded RGBA               */
static uint8_t dec_scr[8u << 20];     /* scratch for png_decode     */

static uint32_t rs = 0x12345678u;     /* deterministic xorshift32   */
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static int failures = 0;

/* Fill rgb_buf[w*h*3] using a pattern selector, then run a full encode/decode
 * round-trip and assert it reproduces the source exactly. */
static void roundtrip(const char *name, int w, int h, int pattern) {
    long npix = (long)w * h;
    for (long i = 0; i < npix; i++) {
        uint8_t r, g, b;
        switch (pattern) {
            case 0:  /* solid colour */
                r = 0x20; g = 0xA0; b = 0xF0; break;
            case 1:  /* horizontal/vertical gradient */
                r = (uint8_t)((i % w) * 255 / (w > 1 ? w - 1 : 1));
                g = (uint8_t)((i / w) * 255 / (h > 1 ? h - 1 : 1));
                b = (uint8_t)((r + g) / 2); break;
            case 2:  /* deterministic noise */
                r = (uint8_t)xr(); g = (uint8_t)xr(); b = (uint8_t)xr(); break;
            case 3:  /* black */
                r = g = b = 0; break;
            case 4:  /* white */
                r = g = b = 255; break;
            default: /* per-pixel index pattern (stresses exact byte mapping) */
                r = (uint8_t)(i * 7); g = (uint8_t)(i * 13 + 1); b = (uint8_t)(i * 31 + 2);
                break;
        }
        rgb_buf[i*3+0] = r; rgb_buf[i*3+1] = g; rgb_buf[i*3+2] = b;
    }

    int n = png_encode(rgb_buf, w, h, png_buf, sizeof png_buf, enc_scr, sizeof enc_scr);
    if (n <= 0) {
        printf("  FAIL %-14s (%dx%d): png_encode returned %d\n", name, w, h, n);
        failures++; return;
    }

    int dw = -1, dh = -1;
    int rc = png_decode(png_buf, n, dec_rgba, sizeof dec_rgba,
                        dec_scr, sizeof dec_scr, &dw, &dh);
    if (rc != 0) {
        printf("  FAIL %-14s (%dx%d): png_decode returned %d\n", name, w, h, rc);
        failures++; return;
    }
    if (dw != w || dh != h) {
        printf("  FAIL %-14s: decoded size %dx%d != %dx%d\n", name, dw, dh, w, h);
        failures++; return;
    }

    /* Every pixel: decoded R,G,B must equal source, alpha must be 255. */
    for (long i = 0; i < npix; i++) {
        const uint8_t *s = rgb_buf  + i * 3;
        const uint8_t *d = dec_rgba + i * 4;
        if (d[0] != s[0] || d[1] != s[1] || d[2] != s[2] || d[3] != 255) {
            printf("  FAIL %-14s (%dx%d): pixel %ld got %02x%02x%02x%02x "
                   "want %02x%02x%02x ff\n", name, w, h, i,
                   d[0], d[1], d[2], d[3], s[0], s[1], s[2]);
            failures++; return;
        }
    }
    printf("  ok   %-14s %4dx%-4d -> %d PNG bytes\n", name, w, h, n);
}

int main(void) {
    printf("PNG encoder round-trip tests (encode -> png_decode, exact RGB):\n");

    /* Tiny / odd sizes that exercise edge cases (single pixel, single row/col,
     * odd dimensions where rowbytes is not a nice multiple). */
    roundtrip("1x1",        1,   1,   0);
    roundtrip("1x1-noise",  1,   1,   2);
    roundtrip("2x2",        2,   2,   5);
    roundtrip("1x7-col",    1,   7,   1);
    roundtrip("7x1-row",    7,   1,   1);
    roundtrip("3x5-odd",    3,   5,   5);
    roundtrip("17x13-odd", 17,  13,   5);

    /* Patterns at a moderate size. */
    roundtrip("solid",     64,  48,   0);
    roundtrip("gradient",  64,  48,   1);
    roundtrip("noise",     64,  48,   2);
    roundtrip("black",     50,  50,   3);
    roundtrip("white",     50,  50,   4);
    roundtrip("index",     40,  40,   5);

    /* The larger image required by the spec, in three flavours. */
    roundtrip("256-solid", 256, 256,  0);
    roundtrip("256-grad",  256, 256,  1);
    roundtrip("256-noise", 256, 256,  2);

    /* ---- bounds: one byte too small must return -1, never overflow ---- */
    {
        int w = 33, h = 17;
        /* Fill with noise (an incompressible-ish payload). */
        rs = 0xDEADBEEFu;
        long npix = (long)w * h;
        for (long i = 0; i < npix * 3; i++) rgb_buf[i] = (uint8_t)xr();

        /* Establish the exact full output size and the exact filtered size. */
        int full = png_encode(rgb_buf, w, h, png_buf, sizeof png_buf,
                              enc_scr, sizeof enc_scr);
        if (full <= 0) { printf("  FAIL bounds: baseline encode = %d\n", full); failures++; }
        long filtered = (1L + (long)w * 3) * h;

        /* outcap one byte too small -> -1 (and ASan verifies no OOB write). */
        int r1 = png_encode(rgb_buf, w, h, png_buf, full - 1,
                           enc_scr, sizeof enc_scr);
        if (r1 != -1) { printf("  FAIL bounds: outcap=%d gave %d, want -1\n", full - 1, r1); failures++; }
        else printf("  ok   outcap-1 (%d) -> -1\n", full - 1);

        /* scratchcap one byte too small -> -1. */
        int r2 = png_encode(rgb_buf, w, h, png_buf, sizeof png_buf,
                           enc_scr, (int)filtered - 1);
        if (r2 != -1) { printf("  FAIL bounds: scratchcap=%ld gave %d, want -1\n", filtered - 1, r2); failures++; }
        else printf("  ok   scratchcap-1 (%ld) -> -1\n", filtered - 1);

        /* Exactly-enough scratch must still succeed. */
        int r3 = png_encode(rgb_buf, w, h, png_buf, sizeof png_buf,
                           enc_scr, (int)filtered);
        if (r3 <= 0) { printf("  FAIL bounds: scratchcap=%ld (exact) gave %d\n", filtered, r3); failures++; }
        else printf("  ok   scratchcap exact (%ld) -> %d\n", filtered, r3);
    }

    /* ---- bad args must return -1 ---- */
    {
        int bad = 0;
        bad |= png_encode(rgb_buf, 0,  5, png_buf, sizeof png_buf, enc_scr, sizeof enc_scr) != -1;
        bad |= png_encode(rgb_buf, 5,  0, png_buf, sizeof png_buf, enc_scr, sizeof enc_scr) != -1;
        bad |= png_encode(rgb_buf, -3, 5, png_buf, sizeof png_buf, enc_scr, sizeof enc_scr) != -1;
        bad |= png_encode(rgb_buf, 5, -3, png_buf, sizeof png_buf, enc_scr, sizeof enc_scr) != -1;
        if (bad) { printf("  FAIL bad-args: expected -1 for w<=0/h<=0\n"); failures++; }
        else printf("  ok   bad args (w<=0, h<=0) -> -1\n");
    }

    /* ---- write one PNG to /tmp/x.png for the external interop loader ---- */
    {
        int w = 200, h = 120;
        for (long y = 0; y < h; y++)
            for (long x = 0; x < w; x++) {
                long i = y * w + x;
                rgb_buf[i*3+0] = (uint8_t)(x * 255 / (w - 1));
                rgb_buf[i*3+1] = (uint8_t)(y * 255 / (h - 1));
                rgb_buf[i*3+2] = (uint8_t)((x ^ y) & 0xFF);
            }
        int n = png_encode(rgb_buf, w, h, png_buf, sizeof png_buf, enc_scr, sizeof enc_scr);
        if (n > 0) {
            FILE *f = fopen("/tmp/x.png", "wb");
            if (f) { fwrite(png_buf, 1, (size_t)n, f); fclose(f);
                     printf("  wrote /tmp/x.png (%dx%d, %d bytes) for interop check\n", w, h, n); }
        } else { printf("  FAIL: could not encode /tmp/x.png (%d)\n", n); failures++; }
    }

    if (failures) { printf("FAIL: %d check(s) failed\n", failures); return 1; }
    printf("PASS: png_encode round-trips exact RGBA, bounds + bad-args return -1 "
           "(ASan/UBSan clean)\n");
    return 0;
}
