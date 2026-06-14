/* rsa.h — RSA PKCS#1 v1.5 SHA-256 signature verification (see rsa.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Parse an RSAPublicKey DER into (n, e) byte pointers aliasing `der`. 0/-1. */
int rsa_pubkey_parse(const uint8_t *der, size_t len,
                     const uint8_t **n, size_t *nl, const uint8_t **e, size_t *el);

/* Verify a PKCS#1 v1.5 / SHA-256 signature. Returns 0 if valid, -1 otherwise. */
int rsa_pkcs1_sha256_verify(const uint8_t *n, size_t nl, const uint8_t *e, size_t el,
                            const uint8_t *sig, size_t sl, const uint8_t hash[32]);

/* Verify a PKCS#1 v1.5 / SHA-384 signature (for cert chains signed with SHA-384). */
int rsa_pkcs1_sha384_verify(const uint8_t *n, size_t nl, const uint8_t *e, size_t el,
                            const uint8_t *sig, size_t sl, const uint8_t hash[48]);

/* Verify an RSASSA-PSS / SHA-256 / MGF1-SHA-256 signature (salt len 32). 0/-1.
 * This is what TLS 1.3's CertificateVerify uses for RSA keys. */
int rsa_pss_sha256_verify(const uint8_t *n, size_t nl, const uint8_t *e, size_t el,
                          const uint8_t *sig, size_t sl, const uint8_t hash[32]);
