#!/bin/sh
# Build the engine's regex (compile + backtracking search) for the host with
# ASan+UBSan and fuzz it. Web-page scripts build regexes from untrusted data and
# run them in-kernel on a guard-page-less stack (this engine's history records
# critical matcher stack-overflows), so a pathological pattern x adversarial
# subject must never OOB, overflow, or hang — the step-budget + depth guard must
# bound every run. The harness #includes js.c (JS_NO_MAIN) and drives
# re_compile + re_search directly. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host regex fuzzer (ASan+UBSan)..."
$CC -std=gnu11 -O1 -g $SAN -fwrapv -Ikernel -Ikernel/include \
    tests/regexfuzz/regexfuzz_test.c -o /tmp/osdev_regexfuzz_test
echo "running regex regression + fuzz..."
if /tmp/osdev_regexfuzz_test; then
    echo "PASS: regex engine (ASan/UBSan clean on ReDoS/malformed compile+search fuzz)"
else
    echo "FAIL: regex fuzz aborted (ASan/UBSan caught a memory error or a check failed)"; exit 1
fi
