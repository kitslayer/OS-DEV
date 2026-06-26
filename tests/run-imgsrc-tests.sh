#!/bin/sh
# Build the img_src_attr helper (srcset/data-src/data-original/data-lazy-src
# fallback resolver from kernel/browser.c) on the host with ASan+UBSan and
# test it over crafted attribute strings + malformed/truncated/random slices.
# It runs kernel-side on untrusted page bytes, where an over-read is silent
# guard-page-less stack corruption. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building img_src_attr host test (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/imgsrc/imgsrc_test.c -o /tmp/osdev_imgsrc_test
echo "running img_src_attr regression + fuzz..."
if /tmp/osdev_imgsrc_test; then
    echo "PASS: img_src_attr (ASan/UBSan clean on srcset/data-src/malformed fuzz)"
else
    echo "FAIL: img_src_attr test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
