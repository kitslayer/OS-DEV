#!/usr/bin/env python3
# gen_barrett_vectors.py — the trusted oracle for barrett_fuzz_test.c, covering
# the two NEW reduction paths added on top of bn_mod (M1536): Barrett reduction
# itself (bn_barrett_init + bn_barrett_reduce) and bn_modexp (now internally
# Barrett-accelerated). Same approach as gen_bignum_vectors.py: Python's
# arbitrary-precision integers as an independent oracle, fixed seed for
# reproducibility.
#
# Two vector kinds, tagged by the first column so one file/harness covers both:
#   R <a_hex> <m_hex> <expected_hex>        expected = a % m, with a < m*m
#                                            (bn_barrett_reduce's documented
#                                            precondition -- the shape every
#                                            real caller, bn_modmul_barrett's
#                                            a*b, always produces)
#   E <base_hex> <exp_hex> <m_hex> <expected_hex>   expected = pow(base, exp, m)
import random
import sys

MAXBITS = 260 * 32  # BN_LIMBS * 32 (kernel/include/bignum.h) -- 8320


def to_hex(n):
    nbytes = max(1, (n.bit_length() + 7) // 8)
    return n.to_bytes(nbytes, "big").hex()


def rand_bits(nbits):
    if nbits <= 0:
        return 0
    v = random.getrandbits(nbits)
    v |= 1 << (nbits - 1)
    return v


def main():
    if len(sys.argv) != 2:
        print("usage: gen_barrett_vectors.py <output-path>", file=sys.stderr)
        return 1
    random.seed(7654321)
    lines = []

    def addR(a, m):
        if m <= 0:
            return
        lines.append(("R", to_hex(a), to_hex(m), to_hex(a % m)))

    def addE(base, exp, m):
        if m <= 0:
            return
        lines.append(("E", to_hex(base), to_hex(exp), to_hex(m), to_hex(pow(base, exp, m))))

    # --- Barrett reduce: hand-picked edge cases ---
    addR(0, 5)
    addR(0, 1)
    addR(4, 5)          # a == m-1
    addR(24, 5)         # a == m*m - 1 (right at the documented a < m^2 boundary)
    addR(1, 1)          # k=1 (single-limb modulus), a<m
    addR(0xFFFFFFFE, 0xFFFFFFFF)                    # single-limb modulus, near-max, a<m
    addR(0xFFFFFFFE * 0xFFFFFFFE, 0xFFFFFFFF)       # k=1, a right at the m^2 scale (x*y, x=y=m-1)
    for mb in (1, 2, 8, 16, 31, 32, 33, 63, 64, 65, 127, 128, 129,
               159, 160, 161, 223, 224, 225, 255, 256, 257,
               383, 384, 385, 511, 512, 2047, 2048, 2049,
               4095, 4096, 4097):       # the real curve/RSA sizes + neighbours
        m = rand_bits(mb) or 1
        # realistic shape: a = x*y for random x,y < m (exactly what
        # bn_modmul_barrett's bn_mul produces)
        for _ in range(3):
            x = random.randrange(m)
            y = random.randrange(m)
            addR(x * y, m)
        # plus boundary probes right at/under/over m*m
        addR(m * m - 1, m)
        addR(max(0, m * m - 2), m)
        if m > 1:
            addR(random.randrange(m), m)   # a < m (Barrett must still work, not just a<m^2)

    # --- Barrett reduce: bulk random, uniform over realistic sizes ---
    # BARRETT_MAX_MBITS: bn_barrett_init needs a (2k+1)-limb scratch value
    # (k = limbs(m)) to fit within BN_LIMBS=260, i.e. k <= 129 -> m <= 4128
    # bits. Real moduli (RSA-4096=128 limbs, P-384=12) are comfortably under
    # this; going past it is bn_modexp's OTHER (fallback-to-bn_mod) path,
    # covered separately below, not this loop.
    BARRETT_MAX_MBITS = 129 * 32
    for _ in range(400):
        mb = random.randint(1, BARRETT_MAX_MBITS)
        m = rand_bits(mb) or 1
        x = random.randrange(m)
        y = random.randrange(m)
        addR(x * y, m)

    # --- modexp: hand-picked edge cases ---
    addE(0, 0, 5)        # 0^0 mod m == 1 (Python convention; must match bn_modexp's)
    addE(0, 5, 7)
    addE(7, 0, 5)
    addE(2, 10, 1)       # mod 1 -> always 0
    addE(3, 65537, 3233)          # textbook-RSA-shaped tiny example
    addE(5, 65537, (1 << 2048) - 189)   # e=65537 (real RSA public exponent) at RSA-2048 scale
    addE(5, 65537, (1 << 4096) - 1)     # e=65537 at RSA-4096 scale (bn_barrett's largest real case)
    # Just past BARRETT_MAX_MBITS (4128 bits, 129 limbs) but still safely
    # within bn_mul's own OWN separate ceiling (squaring needs 2*limbs(m) <=
    # BN_LIMBS=260, i.e. m <= 130 limbs = 4160 bits) -- 4136 bits is 130
    # limbs, 1 over Barrett's cap, comfortably under bn_mul's. bn_modexp must
    # fall back to bn_mod here and still be CORRECT, just not accelerated;
    # this covers the bn_barrett_init()==-1 branch specifically. (Anything
    # bigger, e.g. 4200 bits, starts silently truncating in bn_mul itself --
    # a separate, PRE-EXISTING BN_LIMBS ceiling unrelated to Barrett, not
    # something to probe here.)
    addE(5, 65537, rand_bits(4136))
    addE(rand_bits(4136) >> 1, rand_bits(37), rand_bits(4136))
    for mb in (256, 384, 2048, 3072, 4096):   # P-256/P-384/RSA-2048/3072/4096 modulus sizes
        m = rand_bits(mb) or 1
        for _ in range(3):
            base = random.randrange(m)
            # exponents matching real shapes: e=65537-like (17 bits) and
            # n-2-like (mb bits, all-ones-ish) plus a fully random one
            addE(base, 65537, m)
            addE(base, (1 << mb) - 3, m)
            addE(base, rand_bits(random.randint(1, mb)), m)

    # --- modexp: bulk random ---
    for _ in range(300):
        mb = random.randint(1, 4096)
        eb = random.randint(1, 4096)
        m = rand_bits(mb) or 1
        base = random.randrange(m)
        exp = rand_bits(eb)
        addE(base, exp, m)

    with open(sys.argv[1], "w") as f:
        for parts in lines:
            f.write(" ".join(parts) + "\n")
    nR = sum(1 for p in lines if p[0] == "R")
    nE = sum(1 for p in lines if p[0] == "E")
    print(f"generated {nR} Barrett-reduce + {nE} bn_modexp vectors (Python oracle)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
