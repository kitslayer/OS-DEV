#!/bin/sh
# Build the spreadsheet's formula engine (user/sheeteval.h) for the host with
# ASan+UBSan and -fwrapv (matching the OS-authored userspace build, so signed
# arithmetic wraps — defined — rather than tripping UBSan). The engine is pure
# (cell model + recursive-descent evaluator + recalc + number formatting), so
# it's unit-testable off-target like calc's evaluator. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building spreadsheet engine (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/sheet/sheet_test.c -o /tmp/osdev_sheet_test
echo "running formula-engine regression..."
if /tmp/osdev_sheet_test; then
    echo "PASS: spreadsheet engine (refs/ranges/functions/recalc/circular-refs, ASan/UBSan clean)"
else
    echo "FAIL: sheet test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
