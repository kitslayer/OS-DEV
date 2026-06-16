/* svg.h — a minimal, integer-only SVG rasterizer (see svg.c).
 *
 * Decodes a subset of SVG (untrusted web XML) into a tightly-packed RGBA buffer
 * (`out`, 4 bytes/pixel) using `scratch` as a working area for the per-shape
 * point list. On success returns 0 and sets the width/height; returns -1 on a
 * bad/unsupported/truncated image or a buffer overflow. The canvas is W*H where
 * W,H come from the <svg> width/height (or the viewBox); `out` is initialised to
 * fully-transparent black and shapes are composited over it. Matches the other
 * image-decoder signatures (png_decode/jpeg_decode/gif_decode) so the browser
 * can route SVGs through the same path. No FPU/float — all geometry is 16.16
 * fixed-point. Every parse loop is bounded by `len` and every bitmap write is
 * clamped to the canvas, so adversarial input fails gracefully (never OOB/hang).
 */
#pragma once
#include <stdint.h>

int svg_decode(const uint8_t *data, int len,
               uint8_t *out, int out_cap,
               uint8_t *scratch, int scratch_cap,
               int *ow, int *oh);
