#!/bin/sh
# Build the `ls -l` formatting helpers (user/lsfmt.h: mode string + mtime date
# column) for the host with ASan+UBSan and run their regression. Pure math (no
# syscalls), so it's unit-testable off-target like the other extracted helpers.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building ls -l formatting helpers (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/lsfmt/lsfmt_test.c -o /tmp/osdev_lsfmt_test
echo "running ls -l formatting regression..."
if /tmp/osdev_lsfmt_test; then
    echo "PASS: ls -l formatting (mode string + YYYY-MM-DD HH:MM mtime, ASan/UBSan clean)"
else
    echo "FAIL: lsfmt test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
