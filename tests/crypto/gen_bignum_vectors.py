#!/usr/bin/env python3
# gen_bignum_vectors.py — the trusted oracle for bignum_fuzz_test.c.
#
# Emits "<a_hex> <m_hex> <expected_hex>" lines, one per test vector, where
# expected = a % m computed by Python's arbitrary-precision integers (an
# independent, battle-tested implementation, not anything derived from this
# codebase's own bignum.c). Fixed seed -> deterministic across runs, so this
# is a reproducible regression suite, not flaky fuzzing.
#
# Coverage: hand-picked edge cases (mod by 1, a==m, a<m, all-ones) plus a
# structured sweep of (modulus bit-length, dividend bit-length) pairs centered
# on the sizes bn_mod actually sees in production (256/384/2048/3072/4096-bit
# RSA/ECDSA moduli, and ~2x that for post-multiply reduction), plus bulk
# uniform-random fuzz across the whole supported range (1..8320 bits, the
# BN_LIMBS=260 ceiling in kernel/include/bignum.h).
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
    v |= 1 << (nbits - 1)  # force the exact requested bit length
    return v


def main():
    if len(sys.argv) != 2:
        print("usage: gen_bignum_vectors.py <output-path>", file=sys.stderr)
        return 1
    random.seed(1234567)
    vectors = []

    def add(a, m):
        if m == 0:
            return
        vectors.append((a, m, a % m))

    # --- hand-picked edge cases ---
    add(0, 1)
    add(0, 5)
    add(1, 1)
    add(5, 1)
    add(12345, 12345)
    add(12345, 12346)
    add(12346, 12345)
    add(0xFFFFFFFF, 0xFFFFFFFF)
    add(0xFFFFFFFF, 0x100000000)
    add((1 << MAXBITS) - 1, (1 << (MAXBITS // 2)) - 1)   # max-size all-ones a, half-size all-ones m
    add((1 << MAXBITS) - 1, 1)
    add((1 << MAXBITS) - 1, 3)
    add(1 << (MAXBITS - 1), (1 << (MAXBITS - 1)) - 1)     # a is one bit over m, both near max
    add(2 ** 255 - 19, 2 ** 255 - 19)                     # a == m, the X25519 prime shape
    for k in (1, 2, 3, 4, 8, 16, 31, 32, 33, 63, 64):     # power-of-two-ish and word-boundary moduli
        add(rand_bits(k + 37), 1 << k)
        add(rand_bits(k + 37), (1 << k) - 1)

    # --- structured sweep: real-world modulus sizes x realistic dividend sizes ---
    m_bit_choices = [1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
                      127, 128, 129, 159, 160, 161, 223, 224, 225,
                      255, 256, 257, 383, 384, 385, 511, 512, 513,
                      1023, 1024, 1025, 2047, 2048, 2049,
                      3071, 3072, 3073, 4095, 4096, 4097,
                      MAXBITS - 1, MAXBITS]
    for mb in m_bit_choices:
        m = rand_bits(mb) or 1
        a_bit_choices = {max(1, mb - 1), mb, mb + 1, 2 * mb, 2 * mb + 1, MAXBITS}
        for ab in a_bit_choices:
            ab = min(ab, MAXBITS)
            for _ in range(2):
                add(rand_bits(ab), m)

    # --- bulk uniform-random fuzz across the whole range ---
    for _ in range(600):
        mb = random.randint(1, MAXBITS)
        ab = random.randint(1, MAXBITS)
        m = rand_bits(mb) or 1
        a = rand_bits(ab)
        add(a, m)

    with open(sys.argv[1], "w") as f:
        for a, m, e in vectors:
            f.write(f"{to_hex(a)} {to_hex(m)} {to_hex(e)}\n")
    print(f"generated {len(vectors)} bn_mod vectors (Python arbitrary-precision oracle)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
