/* sha1.h — SHA-1 (RFC 3174), pure and self-contained (only <stdint.h>/<stddef.h>).
 *
 * Added for the WebSocket SERVER handshake (M1848): RFC 6455 requires the server
 * to answer with Sec-WebSocket-Accept = base64(SHA1(key + magic-GUID)). SHA-1 is
 * NOT used for anything security-sensitive here (TLS uses SHA-256 in sha256.h);
 * it's only the WebSocket accept-key digest. Shared verbatim with the host test
 * (tests/sha1). One-shot: sha1_hash(msg, len, out[20]).
 */
#ifndef SHA1_H
#define SHA1_H
#include <stdint.h>
#include <stddef.h>

static inline uint32_t sha1__rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

/* Hash `len` bytes of `data` into the 20-byte big-endian digest `out`. */
static inline void sha1_hash(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint64_t ml = (uint64_t)len * 8;

    /* Allocation-free block driver: iterate over message + padding (0x80, zeros
     * to 56 mod 64, then the 64-bit big-endian bit length), materializing each
     * 64-byte block on the fly. */
    size_t total_padded = len + 1;                   /* message + 0x80 */
    while (total_padded % 64 != 56) total_padded++;  /* pad to 56 mod 64 */
    total_padded += 8;                               /* + 64-bit length */

    for (size_t base = 0; base < total_padded; base += 64) {
        uint8_t b[64];
        for (int i = 0; i < 64; i++) {
            size_t pos = base + i;
            if (pos < len) b[i] = data[pos];
            else if (pos == len) b[i] = 0x80;
            else if (pos < total_padded - 8) b[i] = 0x00;
            else b[i] = (uint8_t)(ml >> (8 * (total_padded - 1 - pos)));   /* big-endian length */
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) | ((uint32_t)b[i*4+2] << 8) | b[i*4+3];
        for (int i = 16; i < 80; i++)
            w[i] = sha1__rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h0, bb = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if      (i < 20) { f = (bb & c) | ((~bb) & d);        k = 0x5A827999; }
            else if (i < 40) { f = bb ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = bb ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t tmp = sha1__rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = sha1__rol(bb, 30); bb = a; a = tmp;
        }
        h0 += a; h1 += bb; h2 += c; h3 += d; h4 += e;
    }

    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(hs[i] >> 24);
        out[i*4+1] = (uint8_t)(hs[i] >> 16);
        out[i*4+2] = (uint8_t)(hs[i] >> 8);
        out[i*4+3] = (uint8_t)(hs[i]);
    }
}
#endif /* SHA1_H */
