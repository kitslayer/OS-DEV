#!/bin/sh
# Generate a deterministic set of bn_mod fuzz vectors with Python's
# arbitrary-precision integers as the oracle (gen_bignum_vectors.py), then
# build kernel/bignum.c on the host with ASan+UBSan (#include'd directly, like
# tests/url/url_test.c does for url.c) and check bn_mod against every vector.
# SKIPs cleanly if python3 is unavailable, matching tests/run-blockdev-tests.sh
# and friends. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: bignum fuzz test (python3 not found)"
    exit 0
fi

VECS=/tmp/osdev_bignum_vectors.txt
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

echo "generating bn_mod fuzz vectors (Python arbitrary-precision oracle)..."
python3 tests/crypto/gen_bignum_vectors.py "$VECS"

echo "building host bignum.c (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/crypto/bignum_fuzz_test.c -o /tmp/osdev_bignum_fuzz_test

echo "running bn_mod fuzz vectors..."
if /tmp/osdev_bignum_fuzz_test "$VECS"; then
    :
else
    echo "FAIL: bignum fuzz test aborted (ASan/UBSan caught a memory error) or a vector mismatched"; exit 1
fi
