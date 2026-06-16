#!/bin/sh
# Build the from-scratch PNG encoder (kernel/png_encode.c) for the host with
# ASan+UBSan and round-trip it through the decoder (kernel/png.c, which uses
# kernel/inflate.c) over solid/gradient/noise/odd-size images — the encoded PNG
# must decode back to the exact original RGB. Also checks outcap/scratchcap
# enforcement (returns -1, no overflow). Reuses the DEFLATE compressor
# (kernel/deflate.c) for the IDAT body. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host PNG encoder + decoder (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include \
    tests/png/png_encode_test.c kernel/png_encode.c kernel/png.c kernel/inflate.c kernel/deflate.c \
    -o /tmp/osdev_pngenc_test
echo "running PNG encoder round-trip + bounds test..."
if /tmp/osdev_pngenc_test; then
    echo "PASS: PNG encoder (round-trips exact RGB, bounds enforced, ASan/UBSan clean)"
else
    echo "FAIL: PNG encoder test aborted (round-trip mismatch or ASan/UBSan memory error)"; exit 1
fi
