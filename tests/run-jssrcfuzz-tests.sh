#!/bin/sh
# Build the full JS engine for the host with ASan+UBSan and fuzz the parse+run
# pipeline on untrusted SOURCE. The browser runs <script>/javascript: from
# arbitrary sites in-kernel on a guard-page-less stack, so malformed/adversarial
# source must never OOB, overflow, or hang — only fail gracefully. The harness
# #includes js.c (JS_NO_MAIN), wires the host DOM/storage stubs, and feeds
# js_run_doc truncations, corruptions, random buffers, and deep nesting.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host JS source fuzzer (ASan+UBSan)..."
$CC -std=gnu11 -O1 -g $SAN -fwrapv -Ikernel -Ikernel/include \
    tests/jssrcfuzz/jssrcfuzz_test.c -o /tmp/osdev_jssrcfuzz_test
echo "running JS source parse+run fuzz..."
if /tmp/osdev_jssrcfuzz_test; then
    echo "PASS: JS source pipeline (ASan/UBSan clean on malformed/adversarial source fuzz)"
else
    echo "FAIL: JS source fuzz aborted (ASan/UBSan caught a memory error or a check failed)"; exit 1
fi
