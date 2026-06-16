/* inflate.h — a small, standalone RFC-1951 DEFLATE decompressor.
 *
 * Decodes raw DEFLATE (no zlib/gzip wrapper) from `src` into `dst`. Returns the
 * number of decoded bytes, or -1 on malformed input / output overflow. Used to
 * decompress PNG image data (the caller strips the 2-byte zlib header first).
 * Depends only on <stdint.h>/<stddef.h> so it can be unit-tested on the host.
 */
#pragma once
#include <stdint.h>

int inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap);

/* Decompress a gzip stream (RFC 1952): validate the magic + method, skip the
 * header (incl. optional FEXTRA/FNAME/FCOMMENT/FHCRC fields), then inflate the
 * DEFLATE body into `dst`. Returns decoded length, or -1 on bad/short input.
 * Bounded: every header read is guarded against `len`. */
int gz_inflate(const uint8_t *gz, int len, uint8_t *dst, int dstcap);
