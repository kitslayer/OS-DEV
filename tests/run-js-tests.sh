#!/bin/sh
# Build the from-scratch JS interpreter for the host (with ASan+UBSan) and run the
# regression suites, diffing against the golden outputs. Exit 0 = pass.
#
# Two golden files: suite.js is the big monolithic suite (one 40MB arena, run to
# completion, only ~350KB headroom — it sits near the cap, so don't append to it
# casually); suite-promise.js is a SECOND run in its own fresh arena for features that
# would otherwise starve it (Promises allocate promise objects + bound resolvers).
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host JS interpreter (ASan+UBSan)..."
$CC -std=gnu11 -DJS_HOSTTEST -O1 $SAN -fwrapv kernel/js.c -o /tmp/osdev_js_test

run_suite() {
    js="$1"; exp="$2"
    echo "running $js..."
    /tmp/osdev_js_test "$js" > /tmp/osdev_js_out 2>&1 || true
    # Guard against a SILENT truncation (e.g. arena OOM aborting mid-run): the suite must
    # run to completion and be error-free, independent of the golden — otherwise a golden
    # regenerated from a broken run would mask the failure as a "match".
    if grep -q "\[js error:" /tmp/osdev_js_out; then
        echo "FAIL: $js produced a JS error (likely arena OOM / truncated run):"
        grep -n "\[js error:" /tmp/osdev_js_out; exit 1
    fi
    if ! tail -1 /tmp/osdev_js_out | grep -q -- "-- done --"; then
        echo "FAIL: $js did not run to completion (last line is not '-- done --'):"
        tail -3 /tmp/osdev_js_out; exit 1
    fi
    if ! diff -u "$exp" /tmp/osdev_js_out; then
        echo "FAIL: $js output differs from $exp (see diff above)"; exit 1
    fi
}

run_suite tests/js/suite.js         tests/js/suite.expected
run_suite tests/js/suite-promise.js tests/js/suite-promise.expected
run_suite tests/js/timers.js        tests/js/timers.expected
echo "PASS: JS engine regression suite (ASan/UBSan clean, ran to completion, output matches golden)"
