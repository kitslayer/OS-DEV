/*
 * ecdsa.c — ECDSA signature verification on NIST P-256 (secp256r1), the EC
 * signature scheme used by modern TLS certificates (ecdsa_secp256r1_sha256).
 * Point arithmetic is Jacobian projective over the prime field, all on the
 * fixed-size bignum (integer-only; no FPU). One final modular inverse via
 * Fermat. Verified on the host against OpenSSL-produced signatures.
 */
#include "ecdsa.h"
#include "bignum.h"
#include "string.h"
#include "smp.h"      /* smp_current_cpu — per-core P/N slots so concurrent verifies (M1528) don't race */

/* P-256 domain parameters (big-endian). */
static const uint8_t P256_P[32]  = {0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t P256_N[32]  = {0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
static const uint8_t P256_GX[32] = {0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96};
static const uint8_t P256_GY[32] = {0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
static const uint8_t P256_B[32]  = {0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b};

/* P-384 domain parameters (big-endian) — same curve shape (a = -3), so the point
 * arithmetic below is reused; only the field prime/order/generator/b differ. */
static const uint8_t P384_P[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff};
static const uint8_t P384_N[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,
    0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73};
static const uint8_t P384_B[48] = {
    0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,0xe3,0xf8,0x2d,0x19,
    0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,
    0xc6,0x56,0x39,0x8d,0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef};
static const uint8_t P384_GX[48] = {
    0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,
    0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,
    0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7};
static const uint8_t P384_GY[48] = {
    0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,
    0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,
    0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f};

/* The field prime and group order — ONE PER CORE (M1528), not a single
 * shared global. ecdsa_verify() sets these once at the top of a call and
 * every helper below (fmul/fadd/fsub/fsqr, then dbl/add/mul) reads them
 * implicitly for the rest of that SAME call; two verifies now run
 * concurrently on different cores (kernel/x509.c parallelizes chain
 * verification via smp_parallel_for), so a single shared P/N would let one
 * core's curve parameters corrupt another's mid-computation. Safe as
 * PER-CORE (rather than threading an explicit modulus parameter through
 * every field/point-arithmetic function) because of smp_parallel_for's own
 * contract: a dispatched job runs start-to-finish on one core with no
 * yielding back to the pool mid-computation, so "this core's slot" and "this
 * call's state" are the same thing for the whole duration of a verify. */
#define ECDSA_MAX_CPUS 16
static bignum g_P[ECDSA_MAX_CPUS], g_N[ECDSA_MAX_CPUS];
static inline bignum *curP(void) { return &g_P[smp_current_cpu() & (ECDSA_MAX_CPUS - 1)]; }
static inline bignum *curN(void) { return &g_N[smp_current_cpu() & (ECDSA_MAX_CPUS - 1)]; }

typedef struct { bignum X, Y, Z; } pt;   /* Jacobian: affine (X/Z^2, Y/Z^3); Z=0 = infinity */

static int is_inf(const pt *a) { return a->Z.n == 1 && a->Z.limb[0] == 0; }

/* field helpers, mod P (this core's slot) */
static void fmul(bignum *o, const bignum *a, const bignum *b) { bn_modmul(o, a, b, curP()); }
static void fadd(bignum *o, const bignum *a, const bignum *b) { bn_modadd(o, a, b, curP()); }
static void fsub(bignum *o, const bignum *a, const bignum *b) { bn_modsub(o, a, b, curP()); }
static void fmul2(bignum *o, const bignum *a) { fadd(o, a, a); }
static void fsqr(bignum *o, const bignum *a) { bn_modmul(o, a, a, curP()); }

/* R = 2*A  (Jacobian doubling for curves with a = -3). */
static void dbl(pt *R, const pt *A) {
    if (is_inf(A)) { *R = *A; return; }
    bignum delta, gamma, beta, alpha, t, t2, x3, y3, z3;
    fsqr(&delta, &A->Z);              /* delta = Z^2 */
    fsqr(&gamma, &A->Y);              /* gamma = Y^2 */
    fmul(&beta, &A->X, &gamma);       /* beta  = X*gamma */
    fsub(&t, &A->X, &delta); fadd(&t2, &A->X, &delta); fmul(&alpha, &t, &t2);
    fmul2(&t, &alpha); fadd(&alpha, &t, &alpha);      /* alpha = 3*(X-delta)*(X+delta) */
    fsqr(&x3, &alpha);
    fmul2(&t, &beta); fmul2(&t, &t); fmul2(&t, &t); fsub(&x3, &x3, &t);  /* X3 = alpha^2 - 8*beta */
    fadd(&t, &A->Y, &A->Z); fsqr(&t, &t); fsub(&t, &t, &gamma); fsub(&z3, &t, &delta); /* Z3=(Y+Z)^2-gamma-delta */
    fmul2(&t, &beta); fmul2(&t, &t); fsub(&t, &t, &x3); fmul(&y3, &alpha, &t);  /* alpha*(4beta - X3) */
    fsqr(&t, &gamma); fmul2(&t, &t); fmul2(&t, &t); fmul2(&t, &t);              /* 8*gamma^2 */
    fsub(&y3, &y3, &t);
    R->X = x3; R->Y = y3; R->Z = z3;
}

/* R = A + B  (Jacobian add; A,B may be infinity or equal). */
static void add(pt *R, const pt *A, const pt *B) {
    if (is_inf(A)) { *R = *B; return; }
    if (is_inf(B)) { *R = *A; return; }
    bignum z1z1, z2z2, u1, u2, s1, s2, t, h, i, j, rr, v, x3, y3, z3;
    fsqr(&z1z1, &A->Z); fsqr(&z2z2, &B->Z);
    fmul(&u1, &A->X, &z2z2); fmul(&u2, &B->X, &z1z1);
    fmul(&t, &B->Z, &z2z2); fmul(&s1, &A->Y, &t);
    fmul(&t, &A->Z, &z1z1); fmul(&s2, &B->Y, &t);
    if (bn_cmp(&u1, &u2) == 0) {
        if (bn_cmp(&s1, &s2) != 0) { bn_zero(&R->X); R->X.limb[0]=1; bn_zero(&R->Y); R->Y.limb[0]=1; bn_zero(&R->Z); return; }
        dbl(R, A); return;
    }
    fsub(&h, &u2, &u1);
    fmul2(&t, &h); fsqr(&i, &t);          /* I = (2H)^2 */
    fmul(&j, &h, &i);
    fsub(&t, &s2, &s1); fmul2(&rr, &t);   /* r = 2*(S2-S1) */
    fmul(&v, &u1, &i);
    fsqr(&x3, &rr); fsub(&x3, &x3, &j); fmul2(&t, &v); fsub(&x3, &x3, &t);  /* X3=r^2-J-2V */
    fsub(&t, &v, &x3); fmul(&y3, &rr, &t);
    fmul(&t, &s1, &j); fmul2(&t, &t); fsub(&y3, &y3, &t);                   /* -2*S1*J */
    fadd(&t, &A->Z, &B->Z); fsqr(&t, &t); fsub(&t, &t, &z1z1); fsub(&t, &t, &z2z2); fmul(&z3, &t, &h);
    R->X = x3; R->Y = y3; R->Z = z3;
}

/* R = k*A  (double-and-add over the bits of k, MSB first). */
/* double-and-add, scanning the top `bits` bits of k (= the curve's order size, so
 * this works for P-256's 256-bit and P-384's 384-bit scalars). */
static void mul(pt *R, const bignum *k, const pt *A, int bits) {
    pt acc; bn_zero(&acc.X); acc.X.limb[0]=1; bn_zero(&acc.Y); acc.Y.limb[0]=1; bn_zero(&acc.Z); /* infinity */
    for (int i = bits - 1; i >= 0; i--) {
        pt t; dbl(&t, &acc); acc = t;
        if ((k->limb[i / 32] >> (i % 32)) & 1) { add(&t, &acc, A); acc = t; }
    }
    *R = acc;
}

/* Curve-generic verify over a NIST prime curve (a = -3). cp/cn/gx/gy/cb are the
 * fl-byte big-endian field prime / order / generator / b; pub = 0x04‖X‖Y
 * (1+2*fl bytes); hash is fl bytes; r/s big-endian. Returns 0 if valid. This
 * core's g_P/g_N slot is set at the top and read for the rest of THIS call
 * only (M1528 — see the slot declaration's comment for why that's safe under
 * concurrent per-core verifies, not just the single-threaded caller this
 * assumed when written). */
static int ecdsa_verify(const uint8_t *cp, const uint8_t *cn, const uint8_t *gx,
                        const uint8_t *gy, const uint8_t *cb, int fl,
                        const uint8_t *pub, size_t publen, const uint8_t *hash,
                        const uint8_t *r, size_t rl, const uint8_t *s, size_t sl) {
    bignum *P = curP(), *N = curN();
    bn_from_bytes(P, cp, fl); bn_from_bytes(N, cn, fl);
    if (publen != (size_t)(1 + 2*fl) || pub[0] != 0x04) return -1;

    bignum R, S, Z, w, u1, u2;
    if (bn_from_bytes(&R, r, rl) || bn_from_bytes(&S, s, sl)) return -1;
    bignum one; bn_zero(&one); one.limb[0] = 1;
    if (bn_cmp(&R, &one) < 0 || bn_cmp(&R, N) >= 0) return -1;   /* 1 <= r < n */
    if (bn_cmp(&S, &one) < 0 || bn_cmp(&S, N) >= 0) return -1;

    bn_from_bytes(&Z, hash, fl);
    if (bn_cmp(&Z, N) >= 0) bn_modsub(&Z, &Z, N, N);           /* z mod n (≤1 sub) */

    bignum nm2; bn_zero(&nm2); bn_from_bytes(&nm2, cn, fl);       /* w = s^(n-2) mod n */
    { bignum two; bn_zero(&two); two.limb[0]=2; bn_modsub(&nm2, &nm2, &two, N); }
    bn_modexp(&w, &S, &nm2, N);
    bn_modmul(&u1, &Z, &w, N);
    bn_modmul(&u2, &R, &w, N);

    pt G, Q, P1, P2, sum;
    bn_from_bytes(&G.X, gx, fl); bn_from_bytes(&G.Y, gy, fl); bn_zero(&G.Z); G.Z.limb[0]=1;
    bn_from_bytes(&Q.X, pub + 1, fl); bn_from_bytes(&Q.Y, pub + 1 + fl, fl); bn_zero(&Q.Z); Q.Z.limb[0]=1;
    /* validate the public key: coordinates < p and the point is ON the curve
     * (y^2 == x^3 - 3x + b mod p) — guards against invalid-curve attacks. */
    if (bn_cmp(&Q.X, P) >= 0 || bn_cmp(&Q.Y, P) >= 0) return -1;
    {   bignum b, x2, x3, t3x, rhs, lhs, three;
        bn_from_bytes(&b, cb, fl);
        fsqr(&x2, &Q.X); fmul(&x3, &x2, &Q.X);
        bn_zero(&three); three.limb[0] = 3; bn_modmul(&t3x, &three, &Q.X, P);
        fsub(&rhs, &x3, &t3x); fadd(&rhs, &rhs, &b);     /* x^3 - 3x + b */
        fsqr(&lhs, &Q.Y);                                 /* y^2 */
        if (bn_cmp(&lhs, &rhs) != 0) return -1;           /* not on the curve */
    }
    mul(&P1, &u1, &G, fl * 8);
    mul(&P2, &u2, &Q, fl * 8);
    add(&sum, &P1, &P2);
    if (is_inf(&sum)) return -1;

    /* affine x = X / Z^2 mod p; then check x mod n == r */
    bignum zinv, z2inv, x, pm2;
    bn_zero(&pm2); bn_from_bytes(&pm2, cp, fl);
    { bignum two; bn_zero(&two); two.limb[0]=2; bn_modsub(&pm2, &pm2, &two, P); }
    bn_modexp(&zinv, &sum.Z, &pm2, P);          /* Z^(p-2) = Z^-1 */
    fmul(&z2inv, &zinv, &zinv);
    fmul(&x, &sum.X, &z2inv);
    if (bn_cmp(&x, N) >= 0) bn_modsub(&x, &x, N, N);   /* x mod n */
    return bn_cmp(&x, &R) == 0 ? 0 : -1;
}

/* Verify: pub = 0x04‖X‖Y (65 bytes), hash[32], r/s = 32-byte big-endian. 0/-1. */
int ecdsa_p256_verify(const uint8_t *pub, size_t publen,
                      const uint8_t hash[32], const uint8_t *r, size_t rl,
                      const uint8_t *s, size_t sl) {
    return ecdsa_verify(P256_P, P256_N, P256_GX, P256_GY, P256_B, 32, pub, publen, hash, r, rl, s, sl);
}

/* Verify on NIST P-384: pub = 0x04‖X‖Y (97 bytes), hash[48], r/s big-endian. */
int ecdsa_p384_verify(const uint8_t *pub, size_t publen,
                      const uint8_t hash[48], const uint8_t *r, size_t rl,
                      const uint8_t *s, size_t sl) {
    return ecdsa_verify(P384_P, P384_N, P384_GX, P384_GY, P384_B, 48, pub, publen, hash, r, rl, s, sl);
}

/* Parse an ECDSA-Sig-Value DER (SEQUENCE { INTEGER r, INTEGER s }) into r/s
 * slices (aliasing `der`). Returns 0, or -1 on malformation. */
static int der_rs(const uint8_t *der, size_t derlen, const uint8_t **rv, size_t *rvl,
                  const uint8_t **sv, size_t *svl) {
    const uint8_t *p = der, *end = der + derlen;
    if (p + 2 > end || *p++ != 0x30) return -1;
    size_t sl; int b = *p++;
    if (b < 0x80) sl = b; else { int nb=b&0x7f; if(nb<1||nb>2||p+nb>end) return -1; sl=0; for(int i=0;i<nb;i++) sl=(sl<<8)|*p++; }
    if (sl > (size_t)(end - p)) return -1;
    end = p + sl;
    for (int w = 0; w < 2; w++) {
        if (p + 2 > end || *p++ != 0x02) return -1;
        size_t il = *p++;                                /* r/s INTEGER: short-form length */
        if (il > (size_t)(end - p)) return -1;
        const uint8_t *next = p + il;                    /* end of this INTEGER's value */
        const uint8_t *v = p; size_t vl = il;
        while (vl > 1 && v[0] == 0) { v++; vl--; }        /* strip ASN.1 sign byte */
        if (w == 0) { *rv = v; *rvl = vl; } else { *sv = v; *svl = vl; }
        p = next;
    }
    return 0;
}

int ecdsa_p256_verify_der(const uint8_t *pub, size_t publen,
                          const uint8_t hash[32], const uint8_t *der, size_t derlen) {
    const uint8_t *rv, *sv; size_t rvl, svl;
    if (der_rs(der, derlen, &rv, &rvl, &sv, &svl) != 0) return -1;
    return ecdsa_p256_verify(pub, publen, hash, rv, rvl, sv, svl);
}

int ecdsa_p384_verify_der(const uint8_t *pub, size_t publen,
                          const uint8_t hash[48], const uint8_t *der, size_t derlen) {
    const uint8_t *rv, *sv; size_t rvl, svl;
    if (der_rs(der, derlen, &rv, &rvl, &sv, &svl) != 0) return -1;
    return ecdsa_p384_verify(pub, publen, hash, rv, rvl, sv, svl);
}
