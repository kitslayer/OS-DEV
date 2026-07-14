/* wsclient.h — RFC 6455 client opening-handshake helpers (M1845).
 *
 * Pure and self-contained (only <stdint.h>/<stddef.h>), shared by the kernel
 * WebSocket transport (net.c, lands in M1846) and the off-target unit test
 * (tests/wsclient, ASan/UBSan). No allocation, no I/O.
 *
 *   ws_base64            — Sec-WebSocket-Key nonce -> base64 (RFC 4648).
 *   ws_build_handshake   — assemble the "GET ... Upgrade: websocket" request.
 *   ws_handshake_status  — pull the HTTP status code out of the 101 response.
 *
 * We do NOT verify the server's Sec-WebSocket-Accept (that would need SHA-1,
 * which the kernel has no other use for); a 101 status + the frame codec's own
 * validation is sufficient for this client. The frame codec proper lives in the
 * companion header wsframe.h.
 */
#ifndef WSCLIENT_H
#define WSCLIENT_H
#include <stdint.h>
#include <stddef.h>

/* Base64-encode `n` bytes of `in` into `out` (NUL-terminated). `out` must hold
 * at least ((n+2)/3)*4 + 1 bytes. Standard alphabet, '=' padding. */
static inline void ws_base64(const uint8_t *in, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0, i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >>  6) & 63]; out[o++] = T[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >>  6) & 63]; out[o++] = '=';
    }
    out[o] = 0;
}

/* Append the NUL-terminated `s` to out[*po..cap); returns 0 on overflow. */
static inline int ws__append(char *out, size_t *po, size_t cap, const char *s) {
    size_t o = *po;
    for (; *s; s++) { if (o + 1 >= cap) { *po = o; return 0; } out[o++] = *s; }
    *po = o; return 1;
}

/* Build the RFC 6455 client opening handshake into `out` (cap bytes, NUL-
 * terminated). `authority` is the Host: value (may carry :port), `path` the
 * request-target, `key` the base64 Sec-WebSocket-Key. Returns the request
 * length, or -1 if it wouldn't fit. */
static inline long ws_build_handshake(const char *authority, const char *path,
                                      const char *key, char *out, size_t cap) {
    size_t o = 0;
    const char *parts[] = {
        "GET ", path, " HTTP/1.1\r\nHost: ", authority,
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ", key,
        "\r\nSec-WebSocket-Version: 13\r\n\r\n"
    };
    for (unsigned k = 0; k < sizeof(parts) / sizeof(parts[0]); k++)
        if (!ws__append(out, &o, cap, parts[k])) return -1;
    out[o] = 0;
    return (long)o;
}

/* Parse the HTTP status code from a handshake response (`resp`, `n` bytes).
 * Returns the 3-digit code (101 = upgrade accepted), or -1 if the status line
 * isn't a well-formed "HTTP/1.x NNN". Tolerates multi-digit minor versions. */
static inline int ws_handshake_status(const char *resp, size_t n) {
    if (n < 12) return -1;
    if (resp[0] != 'H' || resp[1] != 'T' || resp[2] != 'T' || resp[3] != 'P' ||
        resp[4] != '/' || resp[5] != '1' || resp[6] != '.') return -1;
    size_t i = 7;
    while (i < n && resp[i] != ' ') i++;        /* skip the minor-version digits */
    while (i < n && resp[i] == ' ') i++;        /* skip the space(s) before the code */
    if (i + 3 > n) return -1;
    if (resp[i]   < '0' || resp[i]   > '9' ||
        resp[i+1] < '0' || resp[i+1] > '9' ||
        resp[i+2] < '0' || resp[i+2] > '9') return -1;
    return (resp[i] - '0') * 100 + (resp[i+1] - '0') * 10 + (resp[i+2] - '0');
}
#endif /* WSCLIENT_H */
