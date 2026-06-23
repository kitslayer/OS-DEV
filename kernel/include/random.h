/*
 * random.h — the kernel CSPRNG (M1072).
 *
 * A ChaCha20 "fast-key-erasure" generator seeded from the CPU's hardware
 * entropy (RDSEED/RDRAND when present, a TSC-jitter fallback otherwise) and
 * periodically reseeded. Backs /dev/random + /dev/urandom and the getrandom()
 * syscall, and is a real upgrade over the previous xorshift toy — it also gives
 * the TLS stack genuinely unpredictable key/nonce material.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

void random_init(void);                      /* gather entropy + seed (call once at boot) */
void random_bytes(void *buf, size_t len);    /* fill buf with cryptographically-strong bytes */
int  random_has_hw(void);                    /* 1 if seeded from RDSEED/RDRAND, 0 if TSC-only */
