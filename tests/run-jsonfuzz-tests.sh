#!/bin/sh
# Build the engine's JSON.parse for the host with ASan+UBSan and fuzz it. The
# browser parses untrusted server/API JSON in-kernel on a guard-page-less stack,
# so a malformed/truncated/deeply-nested document must never OOB-read or overflow.
# The harness #includes js.c (defining JS_NO_MAIN to suppress its host main) and
# drives nat_json_parse directly over exactly-sized buffers. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host JSON.parse fuzzer (ASan+UBSan)..."
$CC -std=gnu11 -O1 -g $SAN -fwrapv -Ikernel -Ikernel/include \
    tests/jsonfuzz/jsonfuzz_test.c -o /tmp/osdev_jsonfuzz_test
echo "running JSON.parse regression + fuzz..."
if /tmp/osdev_jsonfuzz_test; then
    echo "PASS: JSON.parse (ASan/UBSan clean on malformed/truncated/deep fuzz)"
else
    echo "FAIL: JSON.parse fuzz aborted (ASan/UBSan caught a memory error or a check failed)"; exit 1
fi
