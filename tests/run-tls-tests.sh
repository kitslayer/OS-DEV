#!/bin/sh
# Build kernel/tls.c's UNTRUSTED record + Certificate-message framing parsers for
# the host with ASan+UBSan and fuzz them against adversarial bytes. tls.c parses
# data straight off a hostile/broken HTTPS server with no stack guard page, so an
# OOB there is kernel corruption. This is the last untrusted parser in the tree
# to get an adversarial harness (x509/json/regex/deflate/zip/png/webp/html/url/
# bignum already have one). The crypto is the REAL code (linked below); only the
# network + a couple kernel hooks are stubbed in tls_test.c. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host TLS framing parser + real crypto (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include \
    tests/tls/tls_test.c \
    kernel/x509.c kernel/bignum.c kernel/ecdsa.c kernel/rsa.c kernel/rootca.c \
    kernel/sha256.c kernel/sha512.c kernel/hkdf.c \
    kernel/aesgcm.c kernel/aes.c kernel/chachapoly.c kernel/x25519.c \
    kernel/url.c \
    -o /tmp/osdev_tls_test
echo "running TLS record + cert-chain framing fuzz..."
if /tmp/osdev_tls_test; then
    echo "PASS: TLS framing parsers (ASan/UBSan clean on adversarial records + cert chains)"
else
    echo "FAIL: TLS framing test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
