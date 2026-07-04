/*
 * aes.c — AES-128 (FIPS-197), with a CTR mode for stream en/decryption.
 *
 * The block cipher is the textbook construction: an initial AddRoundKey, then 9
 * rounds of SubBytes/ShiftRows/MixColumns/AddRoundKey, then a final round
 * without MixColumns. The 16-byte key is expanded into 11 round keys. Verified
 * against the FIPS-197 test vector.
 *
 * CTR mode encrypts a counter block and XORs the result into the data, so
 * encryption and decryption are the same operation — handy for a file tool.
 */
#include "aes.h"
#ifndef AES_RING3
#include "smp.h"      /* smp_parallel_for — split a big CTR buffer across cores (M1529) */
#endif

static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

void aes128_key_expand(const uint8_t key[16], uint8_t rk[176]) {
    static const uint8_t rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
    for (int i = 0; i < 16; i++) rk[i] = key[i];
    int bytes = 16, rc = 0;
    while (bytes < 176) {
        uint8_t t[4];
        for (int i = 0; i < 4; i++) t[i] = rk[bytes - 4 + i];
        if (bytes % 16 == 0) {
            uint8_t tmp = t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;   /* RotWord */
            for (int i = 0; i < 4; i++) t[i] = sbox[t[i]];                   /* SubWord */
            t[0] ^= rcon[rc++];
        }
        for (int i = 0; i < 4; i++) { rk[bytes] = rk[bytes - 16] ^ t[i]; bytes++; }
    }
}

void aes128_encrypt_block(uint8_t s[16], const uint8_t key[16]) {
    uint8_t rk[176]; aes128_key_expand(key, rk);
    aes128_encrypt_block_rk(s, rk);
}

void aes128_encrypt_block_rk(uint8_t s[16], const uint8_t rk[176]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];                  /* AddRoundKey */
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];          /* SubBytes */
        uint8_t t[16];                                          /* ShiftRows */
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                t[r + 4*c] = s[r + 4*((c + r) & 3)];
        for (int i = 0; i < 16; i++) s[i] = t[i];
        if (round != 10) {                                      /* MixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t *p = s + 4*c;
                uint8_t a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                p[0] = xtime(a0) ^ (xtime(a1)^a1) ^ a2 ^ a3;
                p[1] = a0 ^ xtime(a1) ^ (xtime(a2)^a2) ^ a3;
                p[2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3)^a3);
                p[3] = (xtime(a0)^a0) ^ a1 ^ a2 ^ xtime(a3);
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round*16 + i];   /* AddRoundKey */
    }
}

/* Add a (possibly large) block count onto a 128-bit big-endian counter, with
 * carry -- lets a chunk starting at block index `blocks` compute its own
 * starting counter value directly, instead of ticking up from zero. */
static void ctr_add(uint8_t ctr[16], uint64_t blocks) {
    uint64_t carry = blocks;
    for (int i = 15; i >= 0 && carry; i--) {
        uint64_t sum = (uint64_t)ctr[i] + (carry & 0xFF);
        ctr[i] = (uint8_t)sum;
        carry = (carry >> 8) + (sum >> 8);
    }
}

static void ctr_run(uint8_t *data, size_t off, size_t end, const uint8_t rk[176], uint8_t ctr[16]) {
    for (size_t o = off; o < end; o += 16) {
        uint8_t ks[16]; for (int i = 0; i < 16; i++) ks[i] = ctr[i];
        aes128_encrypt_block_rk(ks, rk);
        size_t n = end - o < 16 ? end - o : 16;
        for (size_t i = 0; i < n; i++) data[o + i] ^= ks[i];
        ctr_add(ctr, 1);
    }
}

#ifndef AES_RING3
/* Only worth the IPI-dispatch + join overhead for a real-sized buffer (M1529)
 * -- SYS_crypt's whole-file CTR pass (up to 32MB, kernel/syscall.c) is the
 * motivating case; a typical TLS record or small file just takes the
 * sequential path below untouched. */
#define AES_CTR_PARALLEL_MIN_BLOCKS 4096   /* >= 64 KiB */

struct ctr_ctx { uint8_t *data; size_t len; const uint8_t *rk; uint8_t nonce[16]; };

static void ctr_chunk(int lo, int hi, void *ctxp) {
    struct ctr_ctx *c = ctxp;
    uint8_t ctr[16]; for (int i = 0; i < 16; i++) ctr[i] = c->nonce[i];
    ctr_add(ctr, (uint64_t)lo);
    size_t off = (size_t)lo * 16, end = (size_t)hi * 16;
    if (end > c->len) end = c->len;
    ctr_run(c->data, off, end, c->rk, ctr);
}
#endif

void aes128_ctr(uint8_t *data, size_t len, const uint8_t key[16], const uint8_t nonce[16]) {
    uint8_t rk[176]; aes128_key_expand(key, rk);
    size_t nblocks = (len + 15) / 16;
#ifndef AES_RING3
    if (nblocks > AES_CTR_PARALLEL_MIN_BLOCKS) {
        struct ctr_ctx c = { data, len, rk, {0} };
        for (int i = 0; i < 16; i++) c.nonce[i] = nonce[i];
        smp_parallel_for((int)nblocks, ctr_chunk, &c);
        return;
    }
#endif
    uint8_t ctr[16]; for (int i = 0; i < 16; i++) ctr[i] = nonce[i];
    ctr_run(data, 0, len, rk, ctr);
}
