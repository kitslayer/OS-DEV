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

# Guard against a SILENT truncation (e.g. arena OOM aborting mid-run): the suite must
# run to completion and be error-free, independent of the golden — otherwise a golden
# regenerated from a broken run would mask the failure as a "match".
if grep -q "\[js error:" /tmp/osdev_js_out; then
    echo "FAIL: suite produced a JS error (likely arena OOM / truncated run):"
    grep -n "\[js error:" /tmp/osdev_js_out; exit 1
fi
if ! tail -1 /tmp/osdev_js_out | grep -q -- "-- done --"; then
    echo "FAIL: suite did not run to completion (last line is not '-- done --'):"
    tail -3 /tmp/osdev_js_out; exit 1
fi

if diff -u tests/js/suite.expected /tmp/osdev_js_out; then
    echo "PASS: JS engine regression suite (ASan/UBSan clean, ran to completion, output matches golden)"
else
    echo "FAIL: output differs from tests/js/suite.expected (see diff above)"; exit 1
fi
