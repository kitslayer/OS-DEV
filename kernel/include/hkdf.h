/* hkdf.h — HMAC-SHA256 and HKDF (RFC 2104 / RFC 5869). See hkdf.c. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void hmac_sha256(const uint8_t *key, size_t klen,
                 const uint8_t *msg, size_t mlen, uint8_t out[32]);

void hkdf_extract(const uint8_t *salt, size_t saltlen,
                  const uint8_t *ikm, size_t ikmlen, uint8_t prk[32]);

int  hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t infolen,
                 uint8_t *okm, size_t L);

/* TLS 1.3 key-schedule helpers (RFC 8446 §7.1). */
int  hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *context, size_t ctxlen, uint8_t *out, size_t L);
int  tls13_derive_secret(const uint8_t secret[32], const char *label,
                         const uint8_t thash[32], uint8_t out[32]);
