/* sha512.h — SHA-512 and SHA-384 (FIPS 180-4). 64-bit-word hash, used for
 * certificate signatures that aren't SHA-256. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void sha512(const uint8_t *msg, size_t len, uint8_t out[64]);
void sha384(const uint8_t *msg, size_t len, uint8_t out[48]);
