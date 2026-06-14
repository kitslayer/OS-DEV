/*
 * string.c — freestanding mem/str helpers.
 *
 * GCC is allowed to emit calls to memset/memcpy/memmove/memcmp even with
 * -ffreestanding (e.g. to zero a struct), so these must exist.
 *
 * memcpy/memset copy a machine word (8 bytes) at a time on the aligned fast
 * path, falling back to bytes for the head, the tail, and unequally-aligned
 * copies. This matters a lot: the compositor blits the whole ~3 MB framebuffer
 * through memcpy on every scene change, and the image decoders / FAT I/O lean
 * on both. `may_alias` keeps the word accesses strict-aliasing-correct.
 */
#include "string.h"
#include <stdint.h>

typedef uint64_t __attribute__((may_alias)) word_t;

void *memset(void *dst, int c, size_t n) {
    unsigned char *p = dst;
    unsigned char b = (unsigned char)c;
    if (n >= 8) {
        word_t w = (word_t)b;
        w |= w << 8; w |= w << 16; w |= w << 32;     /* replicate byte to 8 lanes */
        while (n && ((uintptr_t)p & 7u)) { *p++ = b; n--; } /* reach 8-byte alignment */
        while (n >= 8) { *(word_t *)p = w; p += 8; n -= 8; }
    }
    while (n--) *p++ = b;
    return dst;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    /* word-at-a-time only when src and dst share their low 3 alignment bits */
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7u) == 0) {
        while (n && ((uintptr_t)d & 7u)) { *d++ = *s++; n--; }   /* align */
        while (n >= 8) { *(word_t *)d = *(const word_t *)s; d += 8; s += 8; n -= 8; }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n--; x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
