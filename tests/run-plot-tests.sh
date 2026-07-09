#!/bin/sh
# Build the graphing calculator's expression engine (user/ploteval.h) for the
# host with ASan+UBSan and -fwrapv (matching the OS-authored userspace build, so
# signed arithmetic wraps — defined — rather than tripping UBSan). The engine is
# pure (recursive-descent evaluator with the variable x, the math functions from
# dmath.h, precedence, and error handling), so it's unit-testable off-target like
# calc's and sheet's evaluators. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building graphing-calculator engine (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/plot/plot_test.c -o /tmp/osdev_plot_test
echo "running expression-engine regression..."
if /tmp/osdev_plot_test; then
    echo "PASS: graphing-calculator engine (x variable + functions + precedence, ASan/UBSan clean)"
else
    echo "FAIL: plot test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
