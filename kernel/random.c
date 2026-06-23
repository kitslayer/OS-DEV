/*
 * random.c — a ChaCha20 fast-key-erasure CSPRNG (M1072).
 *
 * The previous /dev/random was a TSC-seeded xorshift — fine for jitter, not for
 * keys. This replaces it with the construction OpenBSD's arc4random and Linux's
 * getrandom() use:
 *
 *   - SEED from the CPU's hardware entropy: RDSEED (a true conditioned entropy
 *     source) if the CPU has it, else RDRAND, else a timestamp-counter jitter
 *     loop. The collected words are whitened through SHA-256 into a 256-bit key.
 *   - GENERATE with ChaCha20: each step produces one 64-byte keystream block;
 *     the first 32 bytes overwrite the key ("fast key erasure", giving forward
 *     secrecy — a later key compromise can't recover earlier output) and the
 *     remaining 32 bytes are the random output.
 *   - RESEED periodically by folding fresh hardware/TSC entropy back into the
 *     key through SHA-256, so the state keeps gaining unpredictability.
 *
 * RDRAND/RDSEED are issued only when CPUID advertises them (otherwise the opcode
 * would #UD), so this is safe under hypervisors that don't expose them.
 */
#define __KERNEL__
#include "random.h"
#include "chachapoly.h"
#include "sha256.h"
#include "console.h"
#include <stdint.h>
#include <stddef.h>

static uint8_t  g_key[32];
static uint8_t  g_nonce[12];               /* fixed; key erasure provides the variation */
static int      g_seeded;
static int      g_have_rdrand, g_have_rdseed;
static uint32_t g_bytes_since_reseed;

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static uint64_t rdtsc(void) {
    uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo;
}

/* RDRAND/RDSEED set CF=1 on success; retry a bounded number of times. */
static int rdrand64(uint64_t *v) {
    for (int i = 0; i < 16; i++) {
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) :: "cc");
        if (ok) return 1;
    }
    return 0;
}
static int rdseed64(uint64_t *v) {
    for (int i = 0; i < 64; i++) {
        unsigned char ok;
        __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok) :: "cc");
        if (ok) return 1;
        __asm__ volatile("pause");
    }
    return 0;
}

/* One 64-bit word of entropy: prefer RDSEED, then RDRAND, else a TSC-jitter mix
 * (read the cycle counter across a small variable-latency loop). */
static uint64_t entropy_word(void) {
    uint64_t v;
    if (g_have_rdseed && rdseed64(&v)) return v;
    if (g_have_rdrand && rdrand64(&v)) return v;
    uint64_t acc = rdtsc();
    for (int i = 0; i < 64; i++) {
        for (volatile int j = 0; j < (int)(acc & 7) + 1; j++) { /* data-dependent delay */ }
        acc = (acc << 1 | acc >> 63) ^ rdtsc();
    }
    return acc;
}

static void store64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* Reseed: SHA-256 over the current key plus four fresh entropy words. */
static void random_reseed(void) {
    uint8_t mix[64];
    for (int i = 0; i < 32; i++) mix[i] = g_key[i];
    for (int w = 0; w < 4; w++) store64(mix + 32 + w * 8, entropy_word());
    sha256(mix, sizeof mix, g_key);
    g_bytes_since_reseed = 0;
}

void random_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);          g_have_rdrand = (c >> 30) & 1;   /* CPUID.1:ECX[30] */
    cpuid(7, 0, &a, &b, &c, &d);          g_have_rdseed = (b >> 18) & 1;   /* CPUID.7:EBX[18] */

    /* Gather 512 bits of seed material, then whiten into the 256-bit key. */
    uint8_t seed[64];
    for (int w = 0; w < 8; w++) store64(seed + w * 8, entropy_word());
    sha256(seed, sizeof seed, g_key);
    for (int i = 0; i < 12; i++) g_nonce[i] = 0;
    g_bytes_since_reseed = 0;
    g_seeded = 1;

    kprintf("[ ok ] random: CSPRNG seeded (%s) -> /dev/random, /dev/urandom, getrandom\n",
            g_have_rdseed ? "RDSEED hardware entropy" :
            g_have_rdrand ? "RDRAND hardware entropy" : "TSC jitter (no RDRAND/RDSEED)");
}

int random_has_hw(void) { return g_have_rdseed || g_have_rdrand; }

void random_bytes(void *buf, size_t len) {
    if (!g_seeded) random_init();
    uint8_t *out = (uint8_t *)buf;
    while (len) {
        uint8_t block[64];
        chacha20_keystream(block, sizeof block, g_key, 0, g_nonce);
        for (int i = 0; i < 32; i++) g_key[i] = block[i];   /* fast key erasure */
        size_t n = len < 32 ? len : 32;                     /* the other 32 bytes are output */
        for (size_t i = 0; i < n; i++) out[i] = block[32 + i];
        out += n; len -= n;
        g_bytes_since_reseed += (uint32_t)n;
        if (g_bytes_since_reseed >= (16u << 10)) random_reseed();   /* fold in fresh entropy every 16 KB */
    }
}
