/* jpeg.h — baseline JPEG decoder (see jpeg.c). */
#pragma once
#include <stdint.h>

/* Decode a baseline JPEG into RGBA (alpha always 255). Returns 0 on success,
 * <0 on error. `scratch` holds the per-component sample planes; `out` receives
 * width*height*4 bytes and sets the dimensions. Every input read is checked. */
int jpeg_decode(const uint8_t *data, int len, uint8_t *out, int out_cap,
                uint8_t *scratch, int scratch_cap, int *width, int *height);

/* Peek dimensions + the scratch byte count jpeg_decode needs (no decode). */
int jpeg_probe(const uint8_t *data, int len, int *w, int *h, long *scratch_needed);
