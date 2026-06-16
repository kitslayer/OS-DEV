/* bmp.h — a minimal BMP decoder (uncompressed BI_RGB: 24-bit, 32-bit, and
 * 8-bit palettized; bottom-up or top-down).
 *
 * Decodes a Windows BMP (BITMAPFILEHEADER + BITMAPINFOHEADER, dib size >= 40)
 * into `out` (4 bytes/pixel, R,G,B,A — same layout the other decoders emit and
 * the renderer expects), with alpha forced to 255 (BI_RGB defines no alpha).
 * On success returns 0 and sets *w and *h; returns <0 on a bad, truncated, or
 * unsupported (compressed / 1/4/16-bit) image. Parses UNTRUSTED bytes, so every
 * read is bounded against `len` (no guard page in the kernel).
 *
 * This closes the loop with the `screenshot` command (M478), which writes a
 * 24-bit BMP: the browser can now display it via <img src="SHOT.BMP">.
 */
#pragma once
#include <stdint.h>

int bmp_decode(const uint8_t *data, int len,
               uint8_t *out, int out_cap,
               int *w, int *h);
