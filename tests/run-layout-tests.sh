#!/bin/sh
# Build the CSS box-layout engine (kernel/layout.h) for the host with ASan+UBSan
# and run its known-answer regression. Pure (no allocation, integer math, a
# caller-supplied text-measure callback), so the block/inline formatting rules
# are unit-testable off-target like the other extracted engines. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building CSS box-layout engine (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Wall -Wextra -Ikernel tests/layout/layout_test.c -o /tmp/osdev_layout_test
echo "running block/inline layout known-answer tests..."
if /tmp/osdev_layout_test; then
    echo "PASS: CSS box layout (§10.3.3 width constraint + auto margins/centring, border/padding, nested block stacking, §8.3.1 margin collapsing, auto vs explicit height, display:none, inline wrapping, hostile input, ASan/UBSan clean)"
else
    echo "FAIL: layout test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
