/* zip.h — a from-scratch PKZIP/.zip archive EXTRACTOR (no encryption).
 *
 * Parses a standard PKWARE .zip container (APPNOTE) via its CENTRAL DIRECTORY
 * — the robust source of truth that always carries the real compressed and
 * uncompressed sizes plus each entry's local-header offset, sidestepping the
 * streaming data-descriptor ambiguity. Each stored (method 0) or DEFLATE
 * (method 8) member is decoded into a caller-supplied `scratch` buffer (reused
 * one entry at a time) and handed to `emit`; method 8 reuses the kernel's raw
 * RFC-1951 decompressor (inflate(), kernel/inflate.c). Directory entries (names
 * ending in '/') and unsupported compression methods are skipped.
 *
 * Fully bounded for untrusted input: every field offset/size read from the
 * archive is validated against `ziplen` before use, every write is bounded by
 * `scratchcap`, and the central-directory walk is capped by both the entry
 * count and `ziplen`. All multi-byte fields are little-endian. Standalone
 * (only <stdint.h>/<stddef.h>) so it builds in the kernel and unit-tests on the
 * host.
 */
#pragma once
#include <stdint.h>

/* Called once per extracted file. `name`/`namelen` is the archive path (not
 * NUL-terminated), `data`/`datalen` the decompressed contents (datalen may be
 * 0 for an empty file). The buffers are only valid for the duration of the
 * call; copy anything you need to keep. */
typedef void (*zip_emit_fn)(void *ctx, const char *name, int namelen,
                            const uint8_t *data, int datalen);

/* Extract every file from the in-memory archive zip[0..ziplen). For each
 * non-directory entry with a supported method, decode it into scratch[0..
 * scratchcap) and invoke emit(ctx, ...). Returns the number of entries emitted,
 * or -1 if the archive is malformed/truncated (no EOCD, bad signatures, or any
 * field that does not fit within ziplen). */
int zip_extract(const uint8_t *zip, int ziplen,
                zip_emit_fn emit, void *ctx,
                uint8_t *scratch, int scratchcap);
