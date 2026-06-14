/* png.h — a minimal PNG decoder (8-bit, non-interlaced; grayscale / RGB / RGBA).
 *
 * Decodes `data`/`len` into a tightly-packed RGBA buffer (`out`, 4 bytes/pixel),
 * using `scratch` for the inflated+unfiltered scanlines. On success returns 0
 * and sets the width/height outputs; returns <0 on a bad/unsupported image or
 * a buffer overflow.
 * Palette (colour type 3) and interlaced PNGs are not supported.
 */
#pragma once
#include <stdint.h>

int png_decode(const uint8_t *data, int len,
               uint8_t *out, int out_cap,
               uint8_t *scratch, int scratch_cap,
               int *w, int *h);
