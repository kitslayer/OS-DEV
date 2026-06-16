/* tar.h — POSIX/ustar tar archive extractor. */
#pragma once
#include <stdint.h>

/* Called once per regular file: `name` (namelen bytes, not NUL-terminated) and
 * the file's `data` (datalen bytes, pointing into the archive). Same signature
 * as zip.h's emit so a caller can share one callback. */
typedef void (*tar_emit_fn)(void *ctx, const char *name, int namelen,
                            const uint8_t *data, int datalen);

/* Extract a (already-decompressed) ustar archive: walk the 512-byte headers and
 * emit each regular file. Returns the number of files emitted, or -1 on a
 * malformed archive. Bounded: every read is checked against `len`. */
int tar_extract(const uint8_t *tar, int len, tar_emit_fn emit, void *ctx);
