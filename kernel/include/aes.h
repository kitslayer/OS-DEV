/* aes.h — AES-128 in CTR mode (en/decrypt are the same operation). */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* XOR `data` (len bytes) with the AES-128-CTR keystream from key + nonce,
 * in place. Encryption and decryption are identical. */
void aes128_ctr(uint8_t *data, size_t len, const uint8_t key[16], const uint8_t nonce[16]);

/* Encrypt a single 16-byte block in place (used internally; exposed for tests). */
void aes128_encrypt_block(uint8_t state[16], const uint8_t key[16]);

/* Split out so a caller doing many blocks under the same key (aes128_ctr's own
 * loop, aesgcm.c's GCTR) expands the round-key schedule ONCE instead of on
 * every single block (M1529). */
void aes128_key_expand(const uint8_t key[16], uint8_t rk[176]);
void aes128_encrypt_block_rk(uint8_t state[16], const uint8_t rk[176]);
