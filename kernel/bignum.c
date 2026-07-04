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

/* --- Barrett reduction (M1536) -------------------------------------------
 * bn_mod's bit-serial division costs O(bits(x) * limbs(m)) word-ops -- for a
 * squared 384-bit intermediate that's ~768 shift+compare+maybe-subtract
 * rounds on a ~12-limb accumulator, called ~8000x per ECDSA-P384 verify
 * (measured ~65ms; see osdev-bignum-reduction-perf memory). Barrett trades a
 * one-time O(64*limbs(m)^2) reciprocal setup for O(limbs(m)^2) per reduction
 * afterward -- a real win ONLY when many reductions share one modulus, which
 * is exactly ecdsa.c's ~384-iteration scalar multiply and bn_modexp's own
 * bit loop below. bn_mod/bn_modmul are left untouched for one-off callers
 * elsewhere (rsa.c's OTHER call sites, x25519.c, etc.) where a fresh
 * reciprocal would cost as much as it saves. */

/* q = a >> (L*32) (drop the low L limbs). */
static void bn_shr_limbs(bignum *q, const bignum *a, int L) {
    bn_zero(q);
    if (L >= a->n) return;                             /* q = 0 */
    q->n = a->n - L;
    for (int i = 0; i < q->n; i++) q->limb[i] = a->limb[i + L];
    bn_trim(q);
}
/* r = a mod b^(L*32) (keep only the low L limbs). */
static void bn_low_limbs(bignum *r, const bignum *a, int L) {
    bn_zero(r);
    int n = a->n < L ? a->n : L;
    for (int i = 0; i < n; i++) r->limb[i] = a->limb[i];
    r->n = n > 0 ? n : 1;
    bn_trim(r);
}

/* q = floor(a/m), rem = a mod m -- bit-serial (same technique as bn_mod, plus
 * recording the quotient bit). Used ONLY by bn_barrett_init's one-time
 * reciprocal setup (called O(1) times per verify, never in the hot loop), so
 * simplicity matters far more than speed here. */
static void bn_divmod(bignum *q, bignum *rem, const bignum *a, const bignum *m) {
    bn_zero(rem);
    bn_zero(q);
    for (int i = bn_bits(a) - 1; i >= 0; i--) {
        bn_shl1(rem);
        int bit = (a->limb[i / 32] >> (i % 32)) & 1;
        rem->limb[0] |= (uint32_t)bit;
        if (rem->n < 1) rem->n = 1;
        if (bn_cmp(rem, m) >= 0) {
            bn_sub(rem, m);
            q->limb[i / 32] |= (1u << (i % 32));
            if (i / 32 + 1 > q->n) q->n = i / 32 + 1;
        }
    }
    bn_trim(q);
}

/* Precompute mu = floor(b^(2k) / m), b = 2^32, k = limbs(m). Needs a (2k+1)-
 * limb scratch value, so k is capped well under BN_LIMBS -- 0 on success, -1
 * if m is too large for Barrett (the caller must fall back to bn_mod; every
 * REAL modulus this codebase uses, up to RSA-4096's 128 limbs, is far under
 * this ceiling, but bn_modexp is a general-purpose function so this can't
 * just be an assert). */
int bn_barrett_init(bn_barrett *ctx, const bignum *m) {
    ctx->k = m->n;
    int idx = 2 * ctx->k;
    if (idx + 1 >= BN_LIMBS) return -1;
    bignum b2k; bn_zero(&b2k);
    b2k.limb[idx] = 1; b2k.n = idx + 1;
    bignum rem;
    bn_divmod(&ctx->mu, &rem, &b2k, m);
    return 0;
}

/* out = x mod m, x < m^2, using ctx's precomputed reciprocal for m (classic
 * Barrett: q1=x>>(k-1), q3=(q1*mu)>>(k+1), r=(x mod b^(k+1))-(q3*m mod
 * b^(k+1)), then at most ~2 conditional subtractions of m). */
void bn_barrett_reduce(bignum *out, const bignum *x, const bignum *m, const bn_barrett *ctx) {
    int k = ctx->k;
    bignum q1, q3, r1, r2, t;
    bn_shr_limbs(&q1, x, k > 0 ? k - 1 : 0);
    bn_mul(&t, &q1, &ctx->mu);
    bn_shr_limbs(&q3, &t, k + 1);
    bn_low_limbs(&r1, x, k + 1);
    bn_mul(&t, &q3, m);
    bn_low_limbs(&r2, &t, k + 1);
    if (bn_cmp(&r1, &r2) >= 0) { *out = r1; bn_sub(out, &r2); }
    else {
        bignum base; bn_zero(&base); base.limb[k + 1] = 1; base.n = k + 2;
        *out = r1; bn_add(out, &base); bn_sub(out, &r2);
    }
    while (bn_cmp(out, m) >= 0) bn_sub(out, m);
}

void bn_modmul_barrett(bignum *out, const bignum *a, const bignum *b, const bignum *m, const bn_barrett *ctx) {
    bignum t; bn_mul(&t, a, b); bn_barrett_reduce(out, &t, m, ctx);
}

/* out = base^exp mod m  (square-and-multiply). Barrett reciprocal for m is
 * computed ONCE and reused across every reduction in the bit loop -- even
 * RSA verify's short (~17-bit e=65537) loop amortizes the setup cost well;
 * ECDSA's modular-inverse calls (256/384-bit exponents) even more so. */
void bn_modexp(bignum *out, const bignum *base, const bignum *exp, const bignum *m) {
    bn_barrett ctx;
    int have_barrett = bn_barrett_init(&ctx, m) == 0;   /* -1: m too large (not a real case; see bn_barrett_init) */
    bignum result, b, t, r2;
    bn_zero(&result); result.limb[0] = 1;
    /* This ONE call has no loop to amortize against, so use bn_mod (correct
     * for ANY base size) rather than assume the Barrett precondition base <
     * m^2 -- every value inside the loop below IS guaranteed < m (hence <
     * m^2) since it's always the product of two already-reduced operands. */
    bn_mod(&b, base, m);                                /* b = base mod m */
    int bits = bn_bits(exp);
    for (int i = 0; i < bits; i++) {
        if ((exp->limb[i / 32] >> (i % 32)) & 1) {
            bn_mul(&t, &result, &b);
            if (have_barrett) bn_barrett_reduce(&r2, &t, m, &ctx); else bn_mod(&r2, &t, m);
            result = r2;
        }
        bn_mul(&t, &b, &b);
        if (have_barrett) bn_barrett_reduce(&r2, &t, m, &ctx); else bn_mod(&r2, &t, m);
        b = r2;
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
