#!/bin/sh
# Build the HTML entity decoder (extracted into kernel/htmlentity.c) for the host
# with ASan+UBSan and fuzz it. decode_entity reads raw bytes from untrusted page
# HTML in-kernel, so a malformed/truncated entity must never OOB. The harness
# #includes htmlentity.c and drives decode_entity over exactly-sized buffers.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host HTML entity decoder (ASan+UBSan)..."
$CC -std=gnu11 -O1 -g $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/htmlentfuzz/htmlentfuzz_test.c -o /tmp/osdev_htmlent_test
echo "running HTML-entity regression + fuzz..."
if /tmp/osdev_htmlent_test; then
    echo "PASS: HTML entity decoder (ASan/UBSan clean on named/numeric/malformed fuzz)"
else
    echo "FAIL: HTML-entity fuzz aborted (ASan/UBSan caught a memory error or a check failed)"; exit 1
fi
