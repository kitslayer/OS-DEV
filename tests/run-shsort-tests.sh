#!/bin/sh
# Build the shell's `sort` key helpers (user/shsort.h) for the host with
# ASan+UBSan and run their regression. Pure (only gr_lc from shgrep.h), so
# unit-testable off-target like the other extracted shell helpers. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell sort helpers (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/shsort/shsort_test.c -o /tmp/osdev_shsort_test
echo "running sort-helper regression..."
if /tmp/osdev_shsort_test; then
    echo "PASS: shell sort helpers (numeric key w/ decimals, fold-eq, field extraction, ASan/UBSan clean)"
else
    echo "FAIL: shsort test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
