/* x25519.h — Curve25519 (X25519) ECDH key exchange, RFC 7748 (see x25519.c). */
#pragma once
#include <stdint.h>

/* q = scalar * point; all buffers are 32 bytes, little-endian. Returns 0. */
int x25519(uint8_t *q, const uint8_t *scalar, const uint8_t *point);

/* Public key from a secret scalar, i.e. scalar * basepoint(9). */
int x25519_base(uint8_t *pub, const uint8_t *secret);
