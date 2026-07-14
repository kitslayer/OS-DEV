#!/bin/sh
# Build kernel/sha1.h (SHA-1, added for the WebSocket server handshake) for the
# host with ASan+UBSan and run its known-answer regression. Pure, unit-testable
# off-target. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building SHA-1 (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Ikernel tests/sha1/sha1_test.c -o /tmp/osdev_sha1_test
echo "running SHA-1 KAT regression..."
if /tmp/osdev_sha1_test; then
    echo "PASS: SHA-1 (FIPS 180 / RFC 3174 vectors + RFC 6455 accept-key digest, ASan/UBSan clean)"
else
    echo "FAIL: sha1 test aborted (digest mismatch or ASan/UBSan memory error)"; exit 1
fi
