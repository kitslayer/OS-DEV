/* webp.h — VP8L (WebP Lossless) decoder.
 *
 * Decodes a RIFF/WEBP file whose image chunk is VP8L into packed RGBA pixels
 * (4 bytes per pixel, R,G,B,A order — same layout as png/jpeg/gif decoders).
 * For lossy (VP8 ) or any unsupported / malformed input, returns non-zero so
 * the caller can fall back to another decoder.
 *
 * Buffers are CALLER-PROVIDED: no malloc, no kernel headers — the same
 * freestanding idiom used throughout this decoder suite.
 */
#pragma once
#include <stdint.h>

/* Probe a RIFF/WEBP VP8L image.  On success returns 0 and sets *w, *h and
 * *scratch_need (bytes of scratch webp_decode needs).  Non-zero if not VP8L,
 * malformed, or dimensions out of range. */
int webp_probe(const uint8_t *data, int len, int *w, int *h, long *scratch_need);

/* Decode a RIFF/WEBP VP8L image into rgba (w*h*4 bytes, R,G,B,A order).
 * scratch >= probe's scratch_need.  Returns 0 on success (sets *ow, *oh);
 * non-zero on any error (malformed bitstream, bad caps, unsupported). */
int webp_decode(const uint8_t *data, int len,
                uint8_t *rgba, int rgba_cap,
                uint8_t *scratch, int scr_cap,
                int *ow, int *oh);
