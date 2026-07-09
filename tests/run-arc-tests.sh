#!/bin/sh
# Build the archive browser's listing engine (user/arccore.h) for the host with
# ASan+UBSan and -fwrapv (matching the OS-authored userspace build). The engine
# is pure (ZIP central-directory + TAR header parsing -> entry list), so it's
# unit-testable off-target like calc's/sheet's/plot's/gjson's/diff's cores.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fwrapv"
echo 'building archive engine (ASan+UBSan, -fwrapv)...'
$CC -std=gnu11 -O1 $SAN -Iuser tests/arc/arc_test.c -o /tmp/osdev_arc_test
echo "running archive-listing regression..."
if /tmp/osdev_arc_test; then
    echo "PASS: archive engine (ZIP central-dir + TAR headers, ASan/UBSan clean)"
else
    echo "FAIL: arc test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
