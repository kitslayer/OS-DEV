#!/bin/sh
# Build the shell's tr/cut text helpers (user/shtxt.h) for the host with
# ASan+UBSan and run their regression. Pure, so unit-testable off-target like the
# other extracted shell helpers. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell tr/cut helpers (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/shtxt/shtxt_test.c -o /tmp/osdev_shtxt_test
echo "running tr/cut-helper regression..."
if /tmp/osdev_shtxt_test; then
    echo "PASS: shell tr/cut helpers (SET expansion w/ ranges, cut range membership, ASan/UBSan clean)"
else
    echo "FAIL: shtxt test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
