#!/bin/sh
# Build the from-scratch CSS colour parser (kernel/color.c) on the host with
# ASan+UBSan and fuzz parse_color over malformed/truncated/random colour tokens.
# It runs kernel-side on untrusted page bytes (style="" / <style> colour values),
# where an over-read or overflow is silent guard-page-less stack corruption.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host CSS colour parser (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/color/color_test.c -o /tmp/osdev_color_test
echo "running colour regression + fuzz..."
if /tmp/osdev_color_test; then
    echo "PASS: color.c parse_color (ASan/UBSan clean on malformed/truncated colour fuzz)"
else
    echo "FAIL: color.c test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
