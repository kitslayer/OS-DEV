/* chachapoly.h — ChaCha20-Poly1305 AEAD (RFC 8439). See chachapoly.c. 96-bit nonce. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void chacha20poly1305_encrypt(uint8_t *out, uint8_t tag[16],
                              const uint8_t *in, size_t len,
                              const uint8_t *aad, size_t aadlen,
                              const uint8_t key[32], const uint8_t nonce[12]);

/* 0 if the tag verifies (out = plaintext), -1 otherwise. */
int chacha20poly1305_decrypt(uint8_t *out, const uint8_t *in, size_t len,
                             const uint8_t *aad, size_t aadlen, const uint8_t tag[16],
                             const uint8_t key[32], const uint8_t nonce[12]);

/* Raw ChaCha20 keystream into out[len] (no AEAD) — the CSPRNG core. */
void chacha20_keystream(uint8_t *out, size_t len, const uint8_t key[32],
                        uint32_t counter, const uint8_t nonce[12]);
