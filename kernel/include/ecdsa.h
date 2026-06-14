/* ecdsa.h — ECDSA signature verification on NIST P-256 (see ecdsa.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Verify with r/s as big-endian byte strings. pub = 0x04‖X‖Y (65 bytes),
 * hash = 32-byte message digest. Returns 0 if valid, -1 otherwise. */
int ecdsa_p256_verify(const uint8_t *pub, size_t publen, const uint8_t hash[32],
                      const uint8_t *r, size_t rl, const uint8_t *s, size_t sl);

/* Same, but parse the DER ECDSA-Sig-Value (SEQUENCE { INTEGER r, INTEGER s }). */
int ecdsa_p256_verify_der(const uint8_t *pub, size_t publen,
                          const uint8_t hash[32], const uint8_t *der, size_t derlen);

/* NIST P-384: pub = 0x04‖X‖Y (97 bytes), hash[48], r/s big-endian. 0 if valid. */
int ecdsa_p384_verify(const uint8_t *pub, size_t publen, const uint8_t hash[48],
                      const uint8_t *r, size_t rl, const uint8_t *s, size_t sl);
int ecdsa_p384_verify_der(const uint8_t *pub, size_t publen,
                          const uint8_t hash[48], const uint8_t *der, size_t derlen);
