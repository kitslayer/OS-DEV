/*
 * bmp.c — a minimal BMP decoder (uncompressed BI_RGB: 24-bit, 32-bit, 8-bit
 * palettized; bottom-up or top-down).
 *
 * A BMP is a 14-byte file header, then a DIB info header (we require the common
 * BITMAPINFOHEADER, size >= 40), an optional palette, then padded pixel rows
 * (each row aligned up to a 4-byte boundary). Colours are stored B,G,R (and for
 * 32-bit a 4th byte that BI_RGB leaves undefined). We emit R,G,B,A — the layout
 * the other decoders produce and the renderer expects — with A forced to 255.
 *
 * This is the read side of the M478 `screenshot` command (which writes 24-bit
 * BMPs): the browser can now show one with <img src="SHOT.BMP">.
 *
 * Like the other decoders this parses UNTRUSTED bytes in the kernel (no stack
 * guard page), so every pixel/palette access is bounded against `len`.
 */
#include "bmp.h"

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int bmp_decode(const uint8_t *data, int len,
               uint8_t *out, int out_cap,
               int *w, int *h) {
    if (len < 54) return -1;                              /* 14 file + 40 DIB minimum */
    if (data[0] != 'B' || data[1] != 'M') return -1;

    uint32_t data_off = rd32(data + 10);                 /* pixel-array offset */
    uint32_t dib      = rd32(data + 14);                 /* DIB header size */
    if (dib < 40) return -1;                             /* require BITMAPINFOHEADER */

    int32_t  iw   = (int32_t)rd32(data + 18);
    int32_t  ih   = (int32_t)rd32(data + 22);
    uint16_t bpp  = rd16(data + 28);
    uint32_t comp = rd32(data + 30);
    if (comp != 0) return -1;                            /* BI_RGB (uncompressed) only */
    if (bpp != 8 && bpp != 24 && bpp != 32) return -1;

    int topdown = 0;
    long width = iw, height = ih;
    if (height < 0) { topdown = 1; height = -height; }   /* negative height = top-down rows */
    if (width < 1 || width > 2048 || height < 1 || height > 2048) return -1;
    if (width * height > 1024 * 1024) return -1;

    long rgba_sz = width * height * 4;
    if (out_cap < rgba_sz) return -1;

    long rowsize = (((long)bpp * width + 31) / 32) * 4;   /* rows padded to 4 bytes */
    /* the whole pixel array must lie within the input */
    if (data_off > (uint32_t)len) return -1;
    if (rowsize * height > (long)len - (long)data_off) return -1;

    /* 8-bit: load the palette (4-byte B,G,R,_ entries) right after the DIB header */
    const uint8_t *pal = 0;
    long ncolors = 0;
    if (bpp == 8) {
        long pal_off = 14 + (long)dib;
        ncolors = (long)rd32(data + 46);                 /* biClrUsed; 0 => 256 */
        if (ncolors <= 0 || ncolors > 256) ncolors = 256;
        if (pal_off < 0 || pal_off + ncolors * 4 > (long)len) return -1;
        pal = data + pal_off;
    }

    for (long y = 0; y < height; y++) {
        long srow_idx = topdown ? y : (height - 1 - y);  /* BMP rows are bottom-up unless top-down */
        const uint8_t *srow = data + data_off + srow_idx * rowsize;
        uint8_t *orow = out + y * width * 4;
        for (long x = 0; x < width; x++) {
            uint8_t R, G, B;
            if (bpp == 24) {
                const uint8_t *p = srow + x * 3;
                B = p[0]; G = p[1]; R = p[2];
            } else if (bpp == 32) {
                const uint8_t *p = srow + x * 4;
                B = p[0]; G = p[1]; R = p[2];            /* 4th byte (BI_RGB) is undefined: ignore */
            } else {                                     /* 8-bit palettized */
                uint8_t idx = srow[x];
                if (idx < ncolors) {
                    const uint8_t *e = pal + (long)idx * 4;
                    B = e[0]; G = e[1]; R = e[2];
                } else {
                    R = G = B = 0;                       /* out-of-range index -> black */
                }
            }
            uint8_t *o = orow + x * 4;
            o[0] = R; o[1] = G; o[2] = B; o[3] = 255;
        }
    }

    *w = (int)width;
    *h = (int)height;
    return 0;
}
