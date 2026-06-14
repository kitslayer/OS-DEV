/*
 * sha256.c — SHA-256 (FIPS 180-4).
 *
 * A textbook implementation: the message is processed in 64-byte blocks, each
 * expanded into a 64-word schedule and run through the 64-round compression.
 * Padding (the 0x80 byte, zero fill to 56 mod 64, then the 64-bit bit length)
 * is synthesized on the fly per block, so no buffer larger than the input is
 * needed. Verified against the standard test vectors (e.g. SHA-256("abc") and
 * SHA-256("hello")).
 *
 * This is also the first piece of a crypto foundation — the same hash a TLS
 * handshake leans on — but it's immediately useful on its own for file
 * checksums (`sha256 <file>`).
 */
#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    uint64_t ml = (uint64_t)len * 8;

    size_t total = len + 1;                 /* 0x80 terminator */
    while (total % 64 != 56) total++;       /* zero-pad to 56 mod 64 */
    total += 8;                             /* 64-bit length */

    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int j = 0; j < 64; j++) {
            size_t idx = off + j;
            if (idx < len)             blk[j] = msg[idx];
            else if (idx == len)       blk[j] = 0x80;
            else if (idx >= total - 8) blk[j] = (uint8_t)(ml >> ((int)(total - 1 - idx) * 8));
            else                       blk[j] = 0;
        }

        uint32_t w[64];
        for (int j = 0; j < 16; j++)
            w[j] = ((uint32_t)blk[j*4] << 24) | ((uint32_t)blk[j*4+1] << 16) |
                   ((uint32_t)blk[j*4+2] << 8) | blk[j*4+3];
        for (int j = 16; j < 64; j++) {
            uint32_t s0 = ror(w[j-15],7) ^ ror(w[j-15],18) ^ (w[j-15] >> 3);
            uint32_t s1 = ror(w[j-2],17) ^ ror(w[j-2],19) ^ (w[j-2] >> 10);
            w[j] = w[j-16] + s0 + w[j-7] + s1;
        }

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int j = 0; j < 64; j++) {
            uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[j] + w[j];
            uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(h[i] >> 24);
        out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >> 8);
        out[i*4+3] = (uint8_t)h[i];
    }
}
