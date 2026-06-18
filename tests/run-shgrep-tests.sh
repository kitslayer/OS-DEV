#!/bin/sh
# Build the shell's grep regex matcher (user/shgrep.h) for the host with
# ASan+UBSan and run its regression + fuzz test. The matcher (^ $ . * [..] \ -i)
# is pure (no syscalls), so it's unit-testable off-target like the kernel's
# extracted parsers (cssprop/url/htmlentity). Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building shell grep matcher (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Iuser tests/shgrep/shgrep_test.c -o /tmp/osdev_shgrep_test
echo "running grep-matcher regression + fuzz..."
if /tmp/osdev_shgrep_test; then
    echo "PASS: shell grep matcher (^ \$ . * [..] \\ + -i, ASan/UBSan clean)"
else
    echo "FAIL: shgrep test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
