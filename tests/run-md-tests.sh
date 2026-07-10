#!/bin/sh
# Build the browser's Markdown->HTML and CSV->HTML converters (kernel/mdconv.h)
# for the host with ASan+UBSan and -fwrapv. Both parse UNTRUSTED local files, so
# the harness fuzzes for OOB reads/writes (every store is capped at `cap`, every
# read bounded by the input length) and regression-tests the HTML output,
# including the M1755 fix that escapes " inside emitted href/src/alt attributes.
# The converters are self-contained (no browser state), so they unit-test
# off-target like calc's/sheet's/plot's/diff's cores. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building md/csv converters (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN tests/md/md_test.c -o /tmp/osdev_md_test
echo "running md/csv regression + fuzz..."
if /tmp/osdev_md_test; then
    echo "PASS: md/csv converters (blocks+inline+links+tables+quote-escape, ASan/UBSan clean)"
else
    echo "FAIL: md/csv test aborted (HTML mismatch or ASan/UBSan memory error)"; exit 1
fi
