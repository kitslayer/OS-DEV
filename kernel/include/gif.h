/* gif.h — a minimal GIF decoder (first frame; LZW; transparency).
 *
 * Decodes the first image of a GIF87a/89a into a tightly-packed RGBA buffer
 * (`out`, 4 bytes/pixel), using `scratch` for the decoded palette indices. On
 * success returns 0 and sets the image width/height; returns <0 on a bad or
 * unsupported image or a buffer overflow. Only the first frame is rendered
 * (animation is ignored).
 */
#pragma once
#include <stdint.h>

int gif_decode(const uint8_t *data, int len,
               uint8_t *out, int out_cap,
               uint8_t *scratch, int scratch_cap,
               int *w, int *h);

/* Animated GIF: decode up to `max_frames` composited full-screen RGBA frames
 * (frame i at out + i*W*H*4) and each frame's delay (centiseconds) into
 * `delays_cs`. Returns the frame count (>=1) and sets W/H, or <0. `scratch`
 * needs W*H*4 (canvas) + W*H (indices). */
int gif_decode_anim(const uint8_t *data, int len, uint8_t *out, int out_cap,
                    uint8_t *scratch, int scratch_cap, int *w, int *h,
                    int *delays_cs, int max_frames);
