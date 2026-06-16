/*
 * tar.c — a from-scratch POSIX/ustar tar archive extractor.
 *
 * A tar archive is a sequence of 512-byte records: for each member a header
 * block (name at offset 0, octal size at 124, type flag at 156, ...) followed
 * by the file's bytes padded up to the next 512-byte boundary. The stream ends
 * at two consecutive all-zero blocks. tar carries no compression itself, so a
 * `.tar.gz` is just this stream after gunzip (the caller decompresses first).
 *
 * BOUNDED FOR UNTRUSTED INPUT: the size field is attacker-controlled, so each
 * member's data span is checked against `len` before it is emitted, and the
 * walk advances by at least one block so it always terminates. Standalone
 * (<stdint.h>/<stddef.h>); no libc.
 */
#include "tar.h"
#include <stddef.h>

/* Parse an n-char octal field (space/NUL padded, the ustar convention). */
static long tar_oct(const uint8_t *p, int n) {
    long v = 0; int i = 0;
    while (i < n && p[i] == ' ') i++;                       /* leading spaces */
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

int tar_extract(const uint8_t *tar, int len, tar_emit_fn emit, void *ctx) {
    if (!tar || !emit || len < 512) return -1;
    int pos = 0, count = 0, zeros = 0;
    while (pos + 512 <= len) {
        const uint8_t *h = tar + pos;
        if (h[0] == 0) {                                    /* empty block; two = end */
            if (++zeros >= 2) break;
            pos += 512; continue;
        }
        zeros = 0;
        int namelen = 0; while (namelen < 100 && h[namelen]) namelen++;   /* name[100] */
        long size = tar_oct(h + 124, 12);                   /* size: octal, 12 bytes */
        char typ = (char)h[156];                            /* type flag */
        pos += 512;                                         /* past the header block */
        if (size < 0 || (long)pos + size > (long)len) break;   /* truncated / bad size */
        if ((typ == '0' || typ == 0) && namelen > 0) {      /* a regular file */
            emit(ctx, (const char *)h, namelen, tar + pos, (int)size);
            count++;
        }
        pos += (int)((size + 511) & ~511L);                 /* skip data, padded to 512 */
    }
    return count;
}
