#!/bin/sh
# Build the shell's statement splitter (user/shsplit.h) for the host with
# ASan+UBSan and run its regression + fuzz test. The splitter is pure (no
# syscalls), so it's unit-testable off-target like shgrep/shmath. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell statement splitter (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/shsplit/shsplit_test.c -o /tmp/osdev_shsplit_test
echo "running segmentation regression + fuzz..."
if /tmp/osdev_shsplit_test; then
    echo "PASS: shell statement splitter (; lists, \$() / if…fi / while…done / for…done, ASan/UBSan clean)"
else
    echo "FAIL: shsplit test aborted (segmentation mismatch or ASan/UBSan memory error)"; exit 1
fi
