#!/bin/sh
# Build the from-scratch ZIP extractor (kernel/zip.c, reusing kernel/inflate.c)
# for the host with ASan+UBSan and run the extraction + fuzz test. Builds a real
# .zip with python3 (and, if present, the system `zip` tool with an archive
# comment) and asserts exact extraction of every file; then fuzzes truncated,
# corrupted, and random inputs and asserts no OOB/UBSan error and no hang.
# Optional $1 reseeds the fuzz RNG. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host ZIP extractor + inflate decoder (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include \
    tests/zip/zip_test.c kernel/zip.c kernel/inflate.c \
    -o /tmp/osdev_zip_test
echo "running ZIP extraction + fuzz/corrupt test..."
if /tmp/osdev_zip_test "$@"; then
    echo "PASS: ZIP extractor (exact extraction + fuzz/corrupt safe, ASan/UBSan clean)"
else
    echo "FAIL: ZIP test aborted (extraction mismatch or ASan/UBSan memory error)"; exit 1
fi
