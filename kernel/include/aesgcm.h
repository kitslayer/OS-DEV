/* aesgcm.h — AES-128-GCM authenticated encryption (see aesgcm.c). 96-bit IV. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void aes128_gcm_encrypt(uint8_t *out, uint8_t tag[16],
                        const uint8_t *in, size_t len,
                        const uint8_t *aad, size_t aadlen,
                        const uint8_t key[16], const uint8_t iv[12]);

/* 0 if the tag verifies (out = plaintext), -1 otherwise. */
int aes128_gcm_decrypt(uint8_t *out, const uint8_t *in, size_t len,
                       const uint8_t *aad, size_t aadlen, const uint8_t tag[16],
                       const uint8_t key[16], const uint8_t iv[12]);
