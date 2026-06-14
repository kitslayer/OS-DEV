/*
 * hkdf.c — HMAC-SHA256 (RFC 2104) and HKDF (RFC 5869): the keyed-hash and
 * key-derivation primitives the TLS key schedule is built from. Another step
 * toward an HTTPS client. Built on the existing one-shot SHA-256.
 *
 * HMAC concatenates the padded key with the message into a single buffer and
 * hashes it (SHA-256 here is one-shot, not streaming), so the message is
 * bounded — which is fine: every HMAC in the TLS 1.3 key schedule is over a
 * small input (a hash, a label, a counter). Verified on the host against the
 * RFC 5869 vectors and OpenSSL.
 */
#include "hkdf.h"
#include "sha256.h"
#include "string.h"

#define BLOCK 64                         /* SHA-256 block size */
/* Bound on the HMAC message. Every HMAC in the TLS 1.3 key schedule is tiny
 * (a transcript hash, an HKDF info block ≤ ~520 B, a label) — well under this;
 * callers MUST stay within it (the one-shot SHA-256 can't stream a longer one). */
#define HMAC_MSG_MAX 1024

void hmac_sha256(const uint8_t *key, size_t klen,
                 const uint8_t *msg, size_t mlen, uint8_t out[32]) {
    uint8_t k0[BLOCK], khash[32];
    if (klen > BLOCK) { sha256(key, klen, khash); key = khash; klen = 32; }
    memset(k0, 0, BLOCK);
    for (size_t i = 0; i < klen && i < BLOCK; i++) k0[i] = key[i];

    if (mlen > HMAC_MSG_MAX) mlen = HMAC_MSG_MAX;          /* guard (callers stay small) */
    uint8_t buf[BLOCK + HMAC_MSG_MAX], inner[32];
    for (int i = 0; i < BLOCK; i++) buf[i] = k0[i] ^ 0x36; /* ipad */
    for (size_t i = 0; i < mlen; i++) buf[BLOCK + i] = msg[i];
    sha256(buf, BLOCK + mlen, inner);

    uint8_t buf2[BLOCK + 32];
    for (int i = 0; i < BLOCK; i++) buf2[i] = k0[i] ^ 0x5c; /* opad */
    for (int i = 0; i < 32; i++) buf2[BLOCK + i] = inner[i];
    sha256(buf2, BLOCK + 32, out);
}

void hkdf_extract(const uint8_t *salt, size_t saltlen,
                  const uint8_t *ikm, size_t ikmlen, uint8_t prk[32]) {
    uint8_t zero[32];
    if (!salt || saltlen == 0) { memset(zero, 0, 32); salt = zero; saltlen = 32; }
    hmac_sha256(salt, saltlen, ikm, ikmlen, prk);          /* PRK = HMAC(salt, IKM) */
}

/* HKDF-Expand: OKM = T(1) | T(2) | ... where T(i) = HMAC(PRK, T(i-1) | info | i). */
int hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t infolen,
                uint8_t *okm, size_t L) {
    if (L > 255 * 32) return -1;
    /* room for the largest info hkdf_expand_label can build (2+1+255+1+255=514). */
    uint8_t t[32], buf[32 + 514 + 1];
    size_t done = 0;
    int n = (int)((L + 31) / 32);
    if (infolen > 514) return -1;
    for (int i = 1; i <= n; i++) {
        size_t p = 0;
        if (i > 1) { for (int j = 0; j < 32; j++) buf[p++] = t[j]; }   /* T(i-1) */
        for (size_t j = 0; j < infolen; j++) buf[p++] = info[j];
        buf[p++] = (uint8_t)i;
        hmac_sha256(prk, 32, buf, p, t);
        size_t take = L - done; if (take > 32) take = 32;
        for (size_t j = 0; j < take; j++) okm[done + j] = t[j];
        done += take;
    }
    return 0;
}

/* TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1): HKDF-Expand with a structured
 * info = uint16(L) ‖ opaque("tls13 "+label) ‖ opaque(context). */
int hkdf_expand_label(const uint8_t secret[32], const char *label,
                      const uint8_t *context, size_t ctxlen, uint8_t *out, size_t L) {
    static const char prefix[] = "tls13 ";
    size_t lablen = strlen(label);
    if (6 + lablen > 255 || ctxlen > 255 || L > 65535) return -1;
    uint8_t info[2 + 1 + 255 + 1 + 255];
    size_t p = 0;
    info[p++] = (uint8_t)(L >> 8); info[p++] = (uint8_t)(L & 0xff);
    info[p++] = (uint8_t)(6 + lablen);
    for (int i = 0; i < 6; i++) info[p++] = (uint8_t)prefix[i];
    for (size_t i = 0; i < lablen; i++) info[p++] = (uint8_t)label[i];
    info[p++] = (uint8_t)ctxlen;
    for (size_t i = 0; i < ctxlen; i++) info[p++] = context[i];
    return hkdf_expand(secret, info, p, out, L);
}

/* TLS 1.3 Derive-Secret(secret, label, messages) = Expand-Label(secret, label,
 * Hash(messages), 32). Here `thash` is the 32-byte transcript hash. */
int tls13_derive_secret(const uint8_t secret[32], const char *label,
                        const uint8_t thash[32], uint8_t out[32]) {
    return hkdf_expand_label(secret, label, thash, 32, out, 32);
}
