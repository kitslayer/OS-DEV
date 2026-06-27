#!/bin/sh
# Build the from-scratch VP8L (WebP Lossless) decoder on the host with
# ASan+UBSan and run regression + fuzz tests.
# Uses cwebp (at /usr/bin/cwebp) to produce lossless WebP files from
# synthesized PPM images, then verifies exact pixel-round-trip correctness.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

echo "building host VP8L decoder (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include \
    tests/webp/webp_test.c \
    -o /tmp/osdev_webp_test

echo "running VP8L tests..."
HAVE_CWEBP=0
HAVE_PYTHON3=0
if command -v cwebp >/dev/null 2>&1; then HAVE_CWEBP=1; fi
if command -v python3 >/dev/null 2>&1; then HAVE_PYTHON3=1; fi

if /tmp/osdev_webp_test "$HAVE_CWEBP" "$HAVE_PYTHON3"; then
    echo "PASS: webp.c VP8L decoder (ASan/UBSan clean on round-trip + malformed-input fuzz)"
else
    echo "FAIL: webp.c test aborted (ASan/UBSan caught a memory error or assertion failed)"; exit 1
fi
