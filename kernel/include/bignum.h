/* bignum.h — fixed-size unsigned big-integer modular arithmetic (see bignum.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define BN_LIMBS 260            /* 32-bit limbs: holds products of RSA-4096 operands */

typedef struct { uint32_t limb[BN_LIMBS]; int n; } bignum;

void bn_zero(bignum *a);
int  bn_from_bytes(bignum *a, const uint8_t *b, size_t len);   /* big-endian; -1 if too big */
void bn_to_bytes(const bignum *a, uint8_t *out, size_t len);   /* big-endian, left-padded */
int  bn_cmp(const bignum *a, const bignum *b);
void bn_modexp(bignum *out, const bignum *base, const bignum *exp, const bignum *m);
void bn_modmul(bignum *out, const bignum *a, const bignum *b, const bignum *m);
void bn_modadd(bignum *out, const bignum *a, const bignum *b, const bignum *m);
void bn_modsub(bignum *out, const bignum *a, const bignum *b, const bignum *m);

/* Barrett reduction (M1536): a precomputed-reciprocal fast path for repeated
 * reductions against the SAME modulus (bn_mod/bn_modmul's bit-serial division
 * is ~30-60x more work per call, but re-deriving the reciprocal fresh every
 * call would cost as much as it saves -- only worth it when the caller can
 * amortize one bn_barrett_init() across many bn_modmul_barrett()s against the
 * same m, e.g. ecdsa.c's ~384-iteration scalar multiply or bn_modexp's own
 * bit loop). x must be < m^2 (true for any a*b with a,b < m). */
typedef struct { bignum mu; int k; } bn_barrett;
int  bn_barrett_init(bn_barrett *ctx, const bignum *m);        /* 0 ok, -1 = m too large for Barrett (caller must fall back) */
void bn_barrett_reduce(bignum *out, const bignum *x, const bignum *m, const bn_barrett *ctx);
void bn_modmul_barrett(bignum *out, const bignum *a, const bignum *b, const bignum *m, const bn_barrett *ctx);
