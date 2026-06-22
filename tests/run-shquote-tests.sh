#!/bin/sh
# Build the shell's quoting pass (user/shquote.h) for the host with ASan+UBSan and
# run its regression + fuzz test. Pure (no syscalls), so it's unit-testable
# off-target like shsplit/shbrace. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell quoting pass (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/shquote/shquote_test.c -o /tmp/osdev_shquote_test
echo "running quoting regression + fuzz..."
if /tmp/osdev_shquote_test; then
    echo "PASS: shell quoting (\"...\"/'...' strip + protect, ASan/UBSan clean)"
else
    echo "FAIL: shquote test aborted (mismatch or ASan/UBSan memory error)"; exit 1
fi
