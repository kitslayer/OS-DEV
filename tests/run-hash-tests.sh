#!/bin/sh
# Build the hash tool's checksum/encoding core (user/hashcore.h) for the host
# with ASan+UBSan and -fwrapv (matching the OS-authored userspace build). The
# core is pure (CRC-32 + Base64), so it's unit-testable off-target like the other
# app cores. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building checksum/encoding core (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/hash/hash_test.c -o /tmp/osdev_hash_test
echo "running CRC-32 + Base64 regression..."
if /tmp/osdev_hash_test; then
    echo "PASS: checksum/encoding core (CRC-32 + Base64, ASan/UBSan clean)"
else
    echo "FAIL: hash test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
