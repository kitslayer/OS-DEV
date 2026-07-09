/* hashcore.h — pure CRC-32 + Base64 for the hash/checksum tool (M1708).
 *
 * The SHA-256/512 in the ghash app come from the kernel via sys_sha256/512
 * (which hash a file by name); CRC-32 and Base64 aren't syscalls, so they live
 * here — pure, no syscalls — and are host-unit-tested by tests/hash exactly like
 * calc/sheet/plot/gjson/diff/arc's cores. Standard IEEE CRC-32 (poly 0xEDB88320,
 * gzip/zlib-compatible) and RFC-4648 Base64.
 */
#ifndef HASHCORE_H
#define HASHCORE_H

/* IEEE CRC-32 (reflected, gzip/zlib) — matches the shell's `crc32`. */
static unsigned hc_crc32(const unsigned char *d, long n) {
    unsigned crc = 0xFFFFFFFFu;
    for (long i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (unsigned)(-(int)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Base64-encode n bytes into out (RFC 4648, '='-padded). Returns the length. */
static const char HC_B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int hc_b64_encode(const unsigned char *d, long n, char *out, int outmax) {
    int p = 0;
    for (long i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16;
        if (i + 1 < n) v |= (unsigned)d[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)d[i + 2];
        if (p > outmax - 5) break;
        out[p++] = HC_B64[(v >> 18) & 63];
        out[p++] = HC_B64[(v >> 12) & 63];
        out[p++] = (i + 1 < n) ? HC_B64[(v >> 6) & 63] : '=';
        out[p++] = (i + 2 < n) ? HC_B64[v & 63] : '=';
    }
    out[p] = 0;
    return p;
}

/* Format a 32-bit value as 8 lowercase hex digits into out (9 bytes incl. NUL). */
static void hc_hex32(unsigned v, char *out) {
    const char *h = "0123456789abcdef";
    for (int i = 0; i < 8; i++) out[i] = h[(v >> ((7 - i) * 4)) & 0xf];
    out[8] = 0;
}

#endif /* HASHCORE_H */
