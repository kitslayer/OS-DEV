#!/bin/sh
# Build the shell's $((expr)) integer evaluator (user/shmath.h) for the host with
# ASan+UBSan and run its regression + fuzz test. The evaluator is pure (the
# sh_var variable hook is stubbed by the test), so it's unit-testable off-target
# like the kernel's extracted parsers (cssprop/url/htmlentity) and shgrep.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building shell $((expr)) evaluator (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/shmath/shmath_test.c -o /tmp/osdev_shmath_test
echo "running arithmetic regression + fuzz..."
if /tmp/osdev_shmath_test; then
    echo "PASS: shell \$((expr)) evaluator (+ - * / %, parens, hex, vars, ASan/UBSan clean)"
else
    echo "FAIL: shmath test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
