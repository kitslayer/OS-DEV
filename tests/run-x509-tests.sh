#!/bin/sh
# Build the from-scratch X.509 certificate parser for the host with ASan+UBSan
# and fuzz it against adversarial DER. Locks the bounds-safety verified in the
# M419-423 security audit (and protects the deferred cert-enforcement work,
# which will touch x509.c). Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host X.509 parser (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include tests/x509/x509_test.c kernel/x509.c -o /tmp/osdev_x509_test
echo "running X.509 parser regression + fuzz..."
if /tmp/osdev_x509_test; then
    echo "PASS: X.509 cert parser (ASan/UBSan clean on crafted DER + fuzz)"
else
    echo "FAIL: X.509 test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
