#!/bin/sh
# Generate a deterministic set of Barrett-reduction + bn_modexp fuzz vectors
# with Python's arbitrary-precision integers as the oracle
# (gen_barrett_vectors.py), then build kernel/bignum.c on the host with
# ASan+UBSan (#include'd directly, like tests/crypto/bignum_fuzz_test.c does)
# and check bn_barrett_reduce/bn_modexp against every vector. SKIPs cleanly if
# python3 is unavailable. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: Barrett/modexp fuzz test (python3 not found)"
    exit 0
fi

VECS=/tmp/osdev_barrett_vectors.txt
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

echo "generating Barrett-reduce + bn_modexp fuzz vectors (Python arbitrary-precision oracle)..."
python3 tests/crypto/gen_barrett_vectors.py "$VECS"

echo "building host bignum.c (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/crypto/barrett_fuzz_test.c -o /tmp/osdev_barrett_fuzz_test

echo "running Barrett-reduce + bn_modexp fuzz vectors..."
if /tmp/osdev_barrett_fuzz_test "$VECS"; then
    :
else
    echo "FAIL: Barrett/modexp fuzz test aborted (ASan/UBSan caught a memory error) or a vector mismatched"; exit 1
fi
