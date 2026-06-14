/*
 * x25519.c — Curve25519 (X25519) scalar multiplication: the elliptic-curve
 * Diffie-Hellman key exchange used by modern TLS (RFC 7748). This is the first
 * crypto primitive toward an eventual HTTPS client.
 *
 * Field arithmetic is over p = 2^255 - 19, elements as 16 signed limbs of ~16
 * bits with 64-bit products — integer-only (the kernel has no FPU), no 128-bit
 * types needed. The implementation follows the public-domain TweetNaCl design,
 * which is compact and well-audited. Verified on the host against the RFC 7748
 * test vectors.
 */
#include "x25519.h"

typedef long long       i64;
typedef unsigned char   u8;
typedef i64             gf[16];

static const gf _121665 = { 0xDB41, 1 };

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;                 /* multiply: c may be negative (<< would be UB) */
    }
}

static void sel25519(gf p, gf q, int b) {
    i64 c = ~(b - 1);
    for (int i = 0; i < 16; i++) { i64 t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

static void pack25519(u8 *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1); m[i-1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2*i] = t[i] & 0xff; o[2*i+1] = t[i] >> 8; }
}

static void unpack25519(gf o, const u8 *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((i64)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void M(gf o, const gf a, const gf b) {
    i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) { S(c, c); if (a != 2 && a != 4) M(c, c, i); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

/* q = scalar * point (both 32-byte little-endian). Returns 0. */
int x25519(uint8_t *q, const uint8_t *scalar, const uint8_t *point) {
    u8 z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;          /* clamp the scalar */
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; i--) {           /* Montgomery ladder */
        i64 r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        A(e, a, c); Z(a, a, c); A(c, b, d); Z(b, b, d);
        S(d, e); S(f, a); M(a, c, a); M(c, b, e);
        A(e, a, c); Z(a, a, c); S(b, a); Z(c, d, f);
        M(a, c, _121665); A(a, a, d); M(c, c, a);
        M(a, d, f); M(d, b, x); S(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    inv25519(c, c);
    M(a, a, c);
    pack25519(q, a);
    return 0;
}

/* Curve25519 base point u=9: derive a public key from a secret scalar. */
int x25519_base(uint8_t *pub, const uint8_t *secret) {
    uint8_t nine[32] = { 9 };
    return x25519(pub, secret, nine);
}
