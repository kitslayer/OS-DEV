#!/bin/sh
# Build the from-scratch JS interpreter for the host (with ASan+UBSan) and run the
# regression suite, diffing against the golden output. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host JS interpreter (ASan+UBSan)..."
$CC -std=gnu11 -DJS_HOSTTEST -O1 $SAN -fwrapv kernel/js.c -o /tmp/osdev_js_test
echo "running tests/js/suite.js..."
/tmp/osdev_js_test tests/js/suite.js > /tmp/osdev_js_out 2>&1 || true
if diff -u tests/js/suite.expected /tmp/osdev_js_out; then
    echo "PASS: JS engine regression suite (ASan/UBSan clean, output matches golden)"
else
    echo "FAIL: output differs from tests/js/suite.expected (see diff above)"; exit 1
fi
