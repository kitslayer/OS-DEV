#!/bin/sh
# Build the from-scratch crypto primitives for the host with ASan+UBSan and check
# each against its published RFC/FIPS known-answer vector. crypto_test.c calls the
# public APIs; the kernel crypto .c files are passed as SEPARATE translation units
# (so file-local statics can't collide) and their mem* calls resolve to libc.
# Locks the TLS 1.3 crypto foundation (SHA/HMAC/HKDF/AES/AES-GCM/ChaCha20-Poly1305/
# X25519) against silent regression. Exit 0 = all vectors match.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building from-scratch crypto KATs (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/crypto/crypto_test.c \
    kernel/sha256.c kernel/sha512.c kernel/aes.c kernel/aesgcm.c \
    kernel/chachapoly.c kernel/hkdf.c kernel/x25519.c \
    -o /tmp/osdev_crypto_test
echo "running crypto known-answer tests..."
if /tmp/osdev_crypto_test; then
    echo "PASS: crypto primitives match published vectors (ASan/UBSan clean)"
else
    echo "FAIL: a crypto KAT mismatched or ASan/UBSan caught a memory error"; exit 1
fi
