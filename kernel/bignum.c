/*
 * bignum.c — fixed-size unsigned big-integer modular arithmetic, enough for RSA
 * signature verification (s^e mod N) toward X.509 / TLS certificate handling.
 *
 * Numbers are little-endian arrays of 32-bit limbs with a tracked length;
 * 64-bit products keep the schoolbook multiply integer-only (kernel has no FPU).
 * Modular reduction is bit-by-bit long division — simple and obviously correct
 * (speed is irrelevant for the occasional certificate check). Host-verified
 * against Python's arbitrary-precision integers and OpenSSL.
 */
#include "bignum.h"
#include "string.h"

static int bn_bits(const bignum *a) {
    for (int i = a->n - 1; i >= 0; i--)
        if (a->limb[i]) {
            uint32_t v = a->limb[i]; int b = 0; while (v) { v >>= 1; b++; }
            return i * 32 + b;
        }
    return 0;
}

static void bn_trim(bignum *a) { while (a->n > 1 && a->limb[a->n - 1] == 0) a->n--; }

void bn_zero(bignum *a) { memset(a->limb, 0, sizeof(a->limb)); a->n = 1; }

/* Load a big-endian byte string. Returns -1 if it doesn't fit. */
int bn_from_bytes(bignum *a, const uint8_t *b, size_t len) {
    bn_zero(a);
    while (len > 0 && b[0] == 0) { b++; len--; }      /* skip leading zeros */
    if (len > (size_t)BN_LIMBS * 4) return -1;
    a->n = (int)((len + 3) / 4); if (a->n == 0) a->n = 1;
    for (size_t i = 0; i < len; i++) {
        size_t pos = len - 1 - i;                      /* byte i from the end */
        a->limb[pos / 4] |= (uint32_t)b[i] << (8 * (pos % 4));
    }
    bn_trim(a);
    return 0;
}

int bn_cmp(const bignum *a, const bignum *b) {
    int n = a->n > b->n ? a->n : b->n;
    for (int i = n - 1; i >= 0; i--) {
        uint32_t x = i < a->n ? a->limb[i] : 0, y = i < b->n ? b->limb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

/* a -= b (assumes a >= b). */
static void bn_sub(bignum *a, const bignum *b) {
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t v = (int64_t)a->limb[i] - (i < b->n ? b->limb[i] : 0) - borrow;
        if (v < 0) { v += (int64_t)1 << 32; borrow = 1; } else borrow = 0;
        a->limb[i] = (uint32_t)v;
    }
    bn_trim(a);
}

static void bn_shl1(bignum *a) {                      /* a <<= 1 */
    uint32_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint32_t nc = a->limb[i] >> 31;
        a->limb[i] = (a->limb[i] << 1) | carry;
        carry = nc;
    }
    if (carry && a->n < BN_LIMBS) a->limb[a->n++] = carry;
}

/* r = a mod m  (bit-by-bit long division; r must differ from a and m). */
static void bn_mod(bignum *r, const bignum *a, const bignum *m) {
    bn_zero(r);
    for (int i = bn_bits(a) - 1; i >= 0; i--) {
        bn_shl1(r);
        int bit = (a->limb[i / 32] >> (i % 32)) & 1;
        r->limb[0] |= (uint32_t)bit;
        if (r->n < 1) r->n = 1;
        if (bn_cmp(r, m) >= 0) bn_sub(r, m);
    }
}

/* a += b. */
static void bn_add(bignum *a, const bignum *b) {
    uint64_t carry = 0;
    int n = (a->n > b->n ? a->n : b->n);
    for (int i = 0; i < n || carry; i++) {
        if (i >= BN_LIMBS) break;
        uint64_t v = (uint64_t)(i < a->n ? a->limb[i] : 0) + (i < b->n ? b->limb[i] : 0) + carry;
        a->limb[i] = (uint32_t)v; carry = v >> 32;
        if (i >= a->n) a->n = i + 1;
    }
    bn_trim(a);
}

/* out = a * b  (schoolbook; out must not alias a or b). */
static void bn_mul(bignum *out, const bignum *a, const bignum *b) {
    bn_zero(out);
    out->n = a->n + b->n; if (out->n > BN_LIMBS) out->n = BN_LIMBS;
    for (int i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->n && i + j < BN_LIMBS; j++) {
            uint64_t cur = (uint64_t)out->limb[i + j] + (uint64_t)a->limb[i] * b->limb[j] + carry;
            out->limb[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        if (i + b->n < BN_LIMBS) out->limb[i + b->n] += (uint32_t)carry;
    }
    bn_trim(out);
}

/* out = base^exp mod m  (square-and-multiply). */
void bn_modexp(bignum *out, const bignum *base, const bignum *exp, const bignum *m) {
    bignum result, b, t, r2;
    bn_zero(&result); result.limb[0] = 1;
    bn_mod(&b, base, m);                               /* b = base mod m */
    int bits = bn_bits(exp);
    for (int i = 0; i < bits; i++) {
        if ((exp->limb[i / 32] >> (i % 32)) & 1) {
            bn_mul(&t, &result, &b); bn_mod(&r2, &t, m); result = r2;
        }
        bn_mul(&t, &b, &b); bn_mod(&r2, &t, m); b = r2;
    }
    *out = result;
}

/* out = a * b mod m. */
void bn_modmul(bignum *out, const bignum *a, const bignum *b, const bignum *m) {
    bignum t; bn_mul(&t, a, b); bn_mod(out, &t, m);
}
/* out = (a + b) mod m  (assumes a, b < m). */
void bn_modadd(bignum *out, const bignum *a, const bignum *b, const bignum *m) {
    bignum t = *a; bn_add(&t, b);
    if (bn_cmp(&t, m) >= 0) bn_sub(&t, m);
    *out = t;
}
/* out = (a - b) mod m  (assumes a, b < m). */
void bn_modsub(bignum *out, const bignum *a, const bignum *b, const bignum *m) {
    bignum t;
    if (bn_cmp(a, b) >= 0) { t = *a; bn_sub(&t, b); }
    else { t = *a; bn_add(&t, m); bn_sub(&t, b); }   /* a + m - b */
    *out = t;
}

/* Serialize to `len` big-endian bytes (left-zero-padded). */
void bn_to_bytes(const bignum *a, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        size_t pos = len - 1 - i;
        out[i] = (pos / 4 < (size_t)a->n) ? (uint8_t)(a->limb[pos / 4] >> (8 * (pos % 4))) : 0;
    }
}
