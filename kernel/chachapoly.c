/*
 * chachapoly.c — ChaCha20-Poly1305 AEAD (RFC 8439), the other mandatory TLS 1.3
 * cipher suite (some servers prefer it over AES-GCM). Self-contained and
 * integer-only (32-bit ARX for ChaCha20; Poly1305 over 2^130-5 with 64-bit
 * products). Verified on the host against OpenSSL.
 */
#include "chachapoly.h"
#include "string.h"

/* ---------------- ChaCha20 (RFC 8439 §2.3) --------------------------------- */

static uint32_t rotl(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }
static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

#define QR(a,b,c,d) \
    a += b; d ^= a; d = rotl(d,16); \
    c += d; b ^= c; b = rotl(b,12); \
    a += b; d ^= a; d = rotl(d, 8); \
    c += d; b ^= c; b = rotl(b, 7);

static void chacha_block(uint32_t out[16], const uint32_t in[16]) {
    for (int i = 0; i < 16; i++) out[i] = in[i];
    for (int r = 0; r < 10; r++) {
        QR(out[0], out[4], out[ 8], out[12]);
        QR(out[1], out[5], out[ 9], out[13]);
        QR(out[2], out[6], out[10], out[14]);
        QR(out[3], out[7], out[11], out[15]);
        QR(out[0], out[5], out[10], out[15]);
        QR(out[1], out[6], out[11], out[12]);
        QR(out[2], out[7], out[ 8], out[13]);
        QR(out[3], out[4], out[ 9], out[14]);
    }
    for (int i = 0; i < 16; i++) out[i] += in[i];
}

static void chacha_init(uint32_t s[16], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    s[0]=0x61707865; s[1]=0x3320646e; s[2]=0x79622d32; s[3]=0x6b206574;
    for (int i = 0; i < 8; i++) s[4+i] = rd32le(key + 4*i);
    s[12] = counter;
    s[13] = rd32le(nonce); s[14] = rd32le(nonce+4); s[15] = rd32le(nonce+8);
}

/* ChaCha20 keystream XOR, starting at the given block counter. */
static void chacha20_xor(uint8_t *out, const uint8_t *in, size_t len,
                         const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
    uint32_t s[16], blk[16];
    chacha_init(s, key, counter, nonce);
    size_t off = 0;
    while (off < len) {
        chacha_block(blk, s);
        s[12]++;                                  /* next block */
        uint8_t ks[64];
        for (int i = 0; i < 16; i++) { ks[4*i]=blk[i]; ks[4*i+1]=blk[i]>>8; ks[4*i+2]=blk[i]>>16; ks[4*i+3]=blk[i]>>24; }
        size_t n = len - off; if (n > 64) n = 64;
        for (size_t i = 0; i < n; i++) out[off+i] = in[off+i] ^ ks[i];
        off += n;
    }
}

/* Raw ChaCha20 keystream (no Poly1305) into out[len] — the stream-cipher core,
 * exposed for the CSPRNG in kernel/random.c (/dev/random + getrandom). The AEAD
 * path above is unchanged; this is purely additive. */
void chacha20_keystream(uint8_t *out, size_t len, const uint8_t key[32],
                        uint32_t counter, const uint8_t nonce[12]) {
    uint32_t s[16], blk[16];
    chacha_init(s, key, counter, nonce);
    size_t off = 0;
    while (off < len) {
        chacha_block(blk, s);
        s[12]++;
        uint8_t ks[64];
        for (int i = 0; i < 16; i++) { ks[4*i]=blk[i]; ks[4*i+1]=blk[i]>>8; ks[4*i+2]=blk[i]>>16; ks[4*i+3]=blk[i]>>24; }
        size_t n = len - off; if (n > 64) n = 64;
        for (size_t i = 0; i < n; i++) out[off+i] = ks[i];
        off += n;
    }
}

/* ---------------- Poly1305 (RFC 8439 §2.5) — donna 32-bit ------------------- */

typedef struct { uint32_t r[5], h[5], pad[4]; } poly;

static void poly_init(poly *p, const uint8_t key[32]) {
    uint32_t t0=rd32le(key), t1=rd32le(key+4), t2=rd32le(key+8), t3=rd32le(key+12);
    p->r[0]= t0                      & 0x3ffffff;
    p->r[1]=((t0>>26)|(t1<< 6))      & 0x3ffff03;
    p->r[2]=((t1>>20)|(t2<<12))      & 0x3ffc0ff;
    p->r[3]=((t2>>14)|(t3<<18))      & 0x3f03fff;
    p->r[4]= (t3>> 8)                & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    p->pad[0]=rd32le(key+16); p->pad[1]=rd32le(key+20); p->pad[2]=rd32le(key+24); p->pad[3]=rd32le(key+28);
}

static void poly_blocks(poly *st, const uint8_t *m, size_t bytes, int final) {
    const uint32_t hibit = final ? 0 : (1u << 24);
    uint32_t r0=st->r[0],r1=st->r[1],r2=st->r[2],r3=st->r[3],r4=st->r[4];
    uint32_t s1=r1*5, s2=r2*5, s3=r3*5, s4=r4*5;
    uint32_t h0=st->h[0],h1=st->h[1],h2=st->h[2],h3=st->h[3],h4=st->h[4];
    while (bytes >= 16) {
        h0 += ( rd32le(m)        ) & 0x3ffffff;
        h1 += ( rd32le(m+3) >> 2 ) & 0x3ffffff;
        h2 += ( rd32le(m+6) >> 4 ) & 0x3ffffff;
        h3 += ( rd32le(m+9) >> 6 ) & 0x3ffffff;
        h4 += ( rd32le(m+12) >> 8 ) | hibit;
        unsigned long long d0=(unsigned long long)h0*r0+(unsigned long long)h1*s4+(unsigned long long)h2*s3+(unsigned long long)h3*s2+(unsigned long long)h4*s1;
        unsigned long long d1=(unsigned long long)h0*r1+(unsigned long long)h1*r0+(unsigned long long)h2*s4+(unsigned long long)h3*s3+(unsigned long long)h4*s2;
        unsigned long long d2=(unsigned long long)h0*r2+(unsigned long long)h1*r1+(unsigned long long)h2*r0+(unsigned long long)h3*s4+(unsigned long long)h4*s3;
        unsigned long long d3=(unsigned long long)h0*r3+(unsigned long long)h1*r2+(unsigned long long)h2*r1+(unsigned long long)h3*r0+(unsigned long long)h4*s4;
        unsigned long long d4=(unsigned long long)h0*r4+(unsigned long long)h1*r3+(unsigned long long)h2*r2+(unsigned long long)h3*r1+(unsigned long long)h4*r0;
        uint32_t c;
        c=(uint32_t)(d0>>26); h0=(uint32_t)d0&0x3ffffff; d1+=c;
        c=(uint32_t)(d1>>26); h1=(uint32_t)d1&0x3ffffff; d2+=c;
        c=(uint32_t)(d2>>26); h2=(uint32_t)d2&0x3ffffff; d3+=c;
        c=(uint32_t)(d3>>26); h3=(uint32_t)d3&0x3ffffff; d4+=c;
        c=(uint32_t)(d4>>26); h4=(uint32_t)d4&0x3ffffff; h0+=c*5;
        c=h0>>26; h0&=0x3ffffff; h1+=c;
        m += 16; bytes -= 16;
    }
    st->h[0]=h0;st->h[1]=h1;st->h[2]=h2;st->h[3]=h3;st->h[4]=h4;
}

static void poly_update(poly *st, uint8_t *leftover, size_t *lo, const uint8_t *m, size_t len) {
    if (*lo) {                                    /* fill the partial buffer first */
        while (*lo < 16 && len) { leftover[(*lo)++] = *m++; len--; }
        if (*lo == 16) { poly_blocks(st, leftover, 16, 0); *lo = 0; }
    }
    if (len >= 16) { size_t n = len & ~(size_t)15; poly_blocks(st, m, n, 0); m += n; len -= n; }
    while (len) { leftover[(*lo)++] = *m++; len--; }
}

static void poly_finish(poly *st, uint8_t *leftover, size_t lo, uint8_t mac[16]) {
    if (lo) { leftover[lo++] = 1; while (lo < 16) leftover[lo++] = 0; poly_blocks(st, leftover, 16, 1); }
    uint32_t h0=st->h[0],h1=st->h[1],h2=st->h[2],h3=st->h[3],h4=st->h[4],c;
    c=h1>>26; h1&=0x3ffffff; h2+=c; c=h2>>26; h2&=0x3ffffff; h3+=c;
    c=h3>>26; h3&=0x3ffffff; h4+=c; c=h4>>26; h4&=0x3ffffff; h0+=c*5;
    c=h0>>26; h0&=0x3ffffff; h1+=c;
    uint32_t g0=h0+5,c2; c2=g0>>26; g0&=0x3ffffff;
    uint32_t g1=h1+c2; c2=g1>>26; g1&=0x3ffffff;
    uint32_t g2=h2+c2; c2=g2>>26; g2&=0x3ffffff;
    uint32_t g3=h3+c2; c2=g3>>26; g3&=0x3ffffff;
    uint32_t g4=h4+c2-(1u<<26);
    uint32_t mask=(g4>>31)-1;                       /* if g4 didn't borrow, use g */
    g0&=mask;g1&=mask;g2&=mask;g3&=mask;g4&=mask;
    mask=~mask; h0=(h0&mask)|g0; h1=(h1&mask)|g1; h2=(h2&mask)|g2; h3=(h3&mask)|g3; h4=(h4&mask)|g4;
    h0=(h0|(h1<<26))&0xffffffff; h1=((h1>>6)|(h2<<20))&0xffffffff;
    h2=((h2>>12)|(h3<<14))&0xffffffff; h3=((h3>>18)|(h4<<8))&0xffffffff;
    unsigned long long f;
    f=(unsigned long long)h0+st->pad[0];           h0=(uint32_t)f;
    f=(unsigned long long)h1+st->pad[1]+(f>>32);   h1=(uint32_t)f;
    f=(unsigned long long)h2+st->pad[2]+(f>>32);   h2=(uint32_t)f;
    f=(unsigned long long)h3+st->pad[3]+(f>>32);   h3=(uint32_t)f;
    for (int i=0;i<4;i++) mac[i]    = h0>>(8*i);
    for (int i=0;i<4;i++) mac[4+i]  = h1>>(8*i);
    for (int i=0;i<4;i++) mac[8+i]  = h2>>(8*i);
    for (int i=0;i<4;i++) mac[12+i] = h3>>(8*i);
}

/* ---------------- AEAD (RFC 8439 §2.8) ------------------------------------- */

static void poly_mac(uint8_t mac[16], const uint8_t key[32],
                     const uint8_t *aad, size_t aadlen, const uint8_t *ct, size_t ctlen) {
    poly st; poly_init(&st, key);
    uint8_t leftover[16]; size_t lo = 0;
    static const uint8_t zero[16] = {0};
    poly_update(&st, leftover, &lo, aad, aadlen);
    if (aadlen % 16) poly_update(&st, leftover, &lo, zero, 16 - (aadlen % 16));
    poly_update(&st, leftover, &lo, ct, ctlen);
    if (ctlen % 16) poly_update(&st, leftover, &lo, zero, 16 - (ctlen % 16));
    uint8_t lens[16];
    for (int i=0;i<8;i++) lens[i]   = (uint8_t)((uint64_t)aadlen >> (8*i));
    for (int i=0;i<8;i++) lens[8+i] = (uint8_t)((uint64_t)ctlen  >> (8*i));
    poly_update(&st, leftover, &lo, lens, 16);
    poly_finish(&st, leftover, lo, mac);
}

void chacha20poly1305_encrypt(uint8_t *out, uint8_t tag[16],
                              const uint8_t *in, size_t len,
                              const uint8_t *aad, size_t aadlen,
                              const uint8_t key[32], const uint8_t nonce[12]) {
    uint8_t polykey[64] = {0};
    chacha20_xor(polykey, polykey, 32, key, 0, nonce);   /* Poly1305 key = block 0 */
    chacha20_xor(out, in, len, key, 1, nonce);           /* encrypt from block 1 */
    poly_mac(tag, polykey, aad, aadlen, out, len);
}

int chacha20poly1305_decrypt(uint8_t *out, const uint8_t *in, size_t len,
                             const uint8_t *aad, size_t aadlen, const uint8_t tag[16],
                             const uint8_t key[32], const uint8_t nonce[12]) {
    uint8_t polykey[64] = {0}, want[16];
    chacha20_xor(polykey, polykey, 32, key, 0, nonce);
    poly_mac(want, polykey, aad, aadlen, in, len);
    int diff = 0;
    for (int i = 0; i < 16; i++) diff |= want[i] ^ tag[i];
    if (diff) return -1;
    chacha20_xor(out, in, len, key, 1, nonce);
    return 0;
}
