#!/bin/sh
# Build the JSON viewer's validator/pretty-printer (user/jsoncore.h) for the host
# with ASan+UBSan and -fwrapv (matching the OS-authored userspace build). The
# engine is pure (recursive-descent validate + pretty-print, error offsets), so
# it's unit-testable off-target like calc's/sheet's/plot's cores. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building JSON engine (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/json/json_test.c -o /tmp/osdev_json_test
echo "running JSON validate/pretty-print regression..."
if /tmp/osdev_json_test; then
    echo "PASS: JSON engine (validate + pretty-print + error offsets, ASan/UBSan clean)"
else
    echo "FAIL: json test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
