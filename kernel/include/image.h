/* image.h — decode any image format the OS understands into RGBA.
 *
 * The definition lives in browser.c (which pulls in the individual decoders).
 * decode_image dispatches by magic bytes (JPEG FF D8 FF, BMP "BM", SVG "<svg",
 * PNG signature, GIF "GIF8") to png/bmp/jpeg/gif/svg_decode, sizing its own
 * scratch + output to the image. Returns a freshly kmalloc'd RGBA buffer
 * (4 bytes/pixel) at the image's native w*h — the caller owns it and must
 * kfree() it — or NULL on any failure (undecodable / OOM / out of range).
 * Dimensions are capped at 2048x2048 and 1M pixels.
 */
#pragma once
#include <stdint.h>

uint8_t *decode_image(const uint8_t *data, int len, int *ow, int *oh);
