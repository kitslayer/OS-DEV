#!/bin/sh
# Build the WebSocket RFC 6455 frame codec (kernel/wsframe.h) for the host with
# ASan+UBSan and run its regression. Pure, so unit-testable off-target like the
# other extracted codecs. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building WebSocket frame codec (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Ikernel tests/wsframe/wsframe_test.c -o /tmp/osdev_wsframe_test
echo "running frame-codec regression..."
if /tmp/osdev_wsframe_test; then
    echo "PASS: WebSocket frame codec (RFC 6455 build/parse, mask, 7/16/64-bit len, fuzz, ASan/UBSan clean)"
else
    echo "FAIL: wsframe test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
