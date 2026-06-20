#!/bin/sh
# Build the shell's cd path resolver (user/normpath.h) for the host with ASan+UBSan
# and run its regression + fuzz test. The resolver is pure and self-contained, so
# it's unit-testable off-target like shgrep/shmath/shsplit. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell path resolver (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/normpath/normpath_test.c -o /tmp/osdev_normpath_test
echo "running path-resolution regression + fuzz..."
if /tmp/osdev_normpath_test; then
    echo "PASS: shell cd path resolver (. / .. / // collapse, absolute/relative, ASan/UBSan clean)"
else
    echo "FAIL: normpath test aborted (resolution mismatch or ASan/UBSan memory error)"; exit 1
fi
