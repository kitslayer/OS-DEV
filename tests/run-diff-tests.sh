#!/bin/sh
# Build the diff viewer's line-diff engine (user/diffcore.h) for the host with
# ASan+UBSan and -fwrapv (matching the OS-authored userspace build). The engine
# is pure (LCS line diff -> context/added/removed entries), so it's unit-testable
# off-target like calc's/sheet's/plot's/gjson's cores. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building diff engine (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/diff/diff_test.c -o /tmp/osdev_diff_test
echo "running line-diff regression..."
if /tmp/osdev_diff_test; then
    echo "PASS: diff engine (LCS line diff, ASan/UBSan clean)"
else
    echo "FAIL: diff test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
