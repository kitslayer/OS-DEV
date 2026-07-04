/*
 * aesgcm.c — AES-128-GCM authenticated encryption (NIST SP 800-38D), built on
 * the existing AES-128 block cipher. The AEAD used by the most widely-supported
 * TLS cipher suites; a step toward an HTTPS client.
 *
 * GCM = counter-mode encryption (GCTR) + a GHASH (GF(2^128) multiply) MAC over
 * the AAD and ciphertext. GHASH is done bit-by-bit (no carryless-multiply
 * instruction needed), which is integer-only and kernel-safe. Verified on the
 * host against the McGrew/NIST GCM test vectors. 96-bit (12-byte) IV only — the
 * size TLS uses.
 */
#include "aesgcm.h"
#include "aes.h"
#include "string.h"

/* X = X * Y in GF(2^128) with the GCM bit ordering (bit 0 = MSB of byte 0). */
static void gf_mul(uint8_t *X, const uint8_t *Y) {
    uint8_t Z[16] = {0}, V[16];
    memcpy(V, Y, 16);
    for (int i = 0; i < 128; i++) {
        if ((X[i >> 3] >> (7 - (i & 7))) & 1)
            for (int j = 0; j < 16; j++) Z[j] ^= V[j];
        int lsb = V[15] & 1;
        for (int j = 15; j > 0; j--) V[j] = (uint8_t)((V[j] >> 1) | ((V[j-1] & 1) << 7));
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xe1;                 /* reduction by R = 11100001 0^120 */
    }
    memcpy(X, Z, 16);
}

/* GHASH: fold `len` bytes (zero-padded to whole blocks) into the accumulator X. */
static void ghash(uint8_t *X, const uint8_t *H, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t n = len - i; if (n > 16) n = 16;
        for (size_t j = 0; j < n; j++) X[j] ^= data[i + j];   /* (rest is zero pad) */
        gf_mul(X, H);
        i += n;
    }
}

static void inc32(uint8_t *ctr) {                /* increment the low 32 bits, big-endian */
    for (int i = 15; i >= 12; i--) { if (++ctr[i]) break; }
}

/* GCTR: encrypt/decrypt `len` bytes from `in` to `out` starting at counter `cb`.
 * Expands the round-key schedule ONCE up front (M1529) instead of re-deriving
 * it from `key` on every single 16-byte block -- a TLS record can be up to
 * ~16KB (1000+ blocks) all under the same key. */
static void gctr(uint8_t *out, const uint8_t *in, size_t len,
                 const uint8_t key[16], uint8_t cb[16]) {
    uint8_t rk[176]; aes128_key_expand(key, rk);
    size_t i = 0;
    while (i < len) {
        uint8_t ks[16]; memcpy(ks, cb, 16);
        aes128_encrypt_block_rk(ks, rk);
        size_t n = len - i; if (n > 16) n = 16;
        for (size_t j = 0; j < n; j++) out[i + j] = in[i + j] ^ ks[j];
        inc32(cb);
        i += n;
    }
}

/* Compute the GCM authentication tag over (aad, ciphertext) into tag[16]. */
static void gcm_tag(uint8_t tag[16], const uint8_t H[16], const uint8_t J0[16],
                    const uint8_t *aad, size_t aadlen,
                    const uint8_t *ct, size_t ctlen, const uint8_t key[16]) {
    uint8_t X[16] = {0};
    ghash(X, H, aad, aadlen);
    ghash(X, H, ct, ctlen);
    uint8_t lenblk[16] = {0};                     /* bit lengths, big-endian 64-bit each */
    uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)ctlen * 8;
    for (int i = 0; i < 8; i++) lenblk[7 - i]  = (uint8_t)(abits >> (i * 8));
    for (int i = 0; i < 8; i++) lenblk[15 - i] = (uint8_t)(cbits >> (i * 8));
    for (int j = 0; j < 16; j++) X[j] ^= lenblk[j];
    gf_mul(X, H);
    uint8_t ej0[16]; memcpy(ej0, J0, 16);
    aes128_encrypt_block(ej0, key);
    for (int j = 0; j < 16; j++) tag[j] = X[j] ^ ej0[j];
}

static void setup(uint8_t H[16], uint8_t J0[16], const uint8_t key[16], const uint8_t iv[12]) {
    memset(H, 0, 16);
    aes128_encrypt_block(H, key);                 /* H = AES_K(0) */
    memcpy(J0, iv, 12);                            /* J0 = IV || 0x00000001 */
    J0[12] = J0[13] = J0[14] = 0; J0[15] = 1;
}

void aes128_gcm_encrypt(uint8_t *out, uint8_t tag[16],
                        const uint8_t *in, size_t len,
                        const uint8_t *aad, size_t aadlen,
                        const uint8_t key[16], const uint8_t iv[12]) {
    uint8_t H[16], J0[16], cb[16];
    setup(H, J0, key, iv);
    memcpy(cb, J0, 16); inc32(cb);                 /* GCTR starts at J0 + 1 */
    gctr(out, in, len, key, cb);
    gcm_tag(tag, H, J0, aad, aadlen, out, len, key);
}

/* Returns 0 if the tag verifies (and `out` holds the plaintext), else -1. */
int aes128_gcm_decrypt(uint8_t *out, const uint8_t *in, size_t len,
                       const uint8_t *aad, size_t aadlen, const uint8_t tag[16],
                       const uint8_t key[16], const uint8_t iv[12]) {
    uint8_t H[16], J0[16], cb[16], want[16];
    setup(H, J0, key, iv);
    gcm_tag(want, H, J0, aad, aadlen, in, len, key);   /* tag over the ciphertext */
    int diff = 0;
    for (int j = 0; j < 16; j++) diff |= want[j] ^ tag[j];   /* constant-time compare */
    if (diff) return -1;
    memcpy(cb, J0, 16); inc32(cb);
    gctr(out, in, len, key, cb);
    return 0;
}
