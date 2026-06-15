#!/bin/sh
# Build the from-scratch image decoders (jpeg/png/gif/inflate) for the host with
# ASan+UBSan and run the regression + fuzz test. Locks the M422 JPEG DRI
# out-of-bounds-read fix and checks all three decoders are bounds-safe against
# adversarial input. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host image decoders (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include \
    tests/img/img_test.c kernel/jpeg.c kernel/png.c kernel/gif.c kernel/inflate.c \
    -o /tmp/osdev_img_test
echo "running image-decoder regression + fuzz..."
if /tmp/osdev_img_test; then
    echo "PASS: image decoders (ASan/UBSan clean on the M422 DRI PoC + fuzz)"
else
    echo "FAIL: image-decoder test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
