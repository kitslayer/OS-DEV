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
