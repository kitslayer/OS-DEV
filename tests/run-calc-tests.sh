#!/bin/sh
# Build the calculator app's expression evaluator (user/calceval.h) for the host
# with ASan+UBSan and -fwrapv (matching the OS-authored userspace build, so the
# signed add/sub/mul accumulation wraps — defined — rather than tripping UBSan's
# signed-overflow check on the intended wraparound). The evaluator is pure, so
# it's unit-testable off-target like shgrep/shmath. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building calculator evaluator (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/calc/calc_test.c -o /tmp/osdev_calc_test
echo "running evaluator regression + fuzz..."
if /tmp/osdev_calc_test; then
    echo "PASS: calculator evaluator (+ - * / % ^ & | << >> ~ ( ) 0x, ASan/UBSan clean)"
else
    echo "FAIL: calc test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
