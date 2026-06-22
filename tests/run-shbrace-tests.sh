#!/bin/sh
# Build the shell's brace expansion (user/shbrace.h) for the host with ASan+UBSan
# and run its regression + fuzz test. The pass is pure (no syscalls), so it's
# unit-testable off-target like shgrep/shmath/shsplit. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell brace expansion (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/shbrace/shbrace_test.c -o /tmp/osdev_shbrace_test
echo "running brace-expansion regression + fuzz..."
if /tmp/osdev_shbrace_test; then
    echo "PASS: shell brace expansion ({a,b}/{1..N} ranges + cartesian, ASan/UBSan clean)"
else
    echo "FAIL: shbrace test aborted (expansion mismatch or ASan/UBSan memory error)"; exit 1
fi
