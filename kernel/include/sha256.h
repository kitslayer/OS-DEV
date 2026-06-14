/* sha256.h — SHA-256 hashing (FIPS 180-4). */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Hash `len` bytes of `data`, writing the 32-byte digest to `out`. */
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);
