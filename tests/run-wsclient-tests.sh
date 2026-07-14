#!/bin/sh
# Build the WebSocket client handshake helpers (kernel/wsclient.h) for the host
# with ASan+UBSan and run their regression. Pure, so unit-testable off-target
# like the frame codec. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building WebSocket handshake helpers (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Ikernel tests/wsclient/wsclient_test.c -o /tmp/osdev_wsclient_test
echo "running handshake-helper regression..."
if /tmp/osdev_wsclient_test; then
    echo "PASS: WebSocket handshake helpers (base64 KAT, request build, 101 status parse, ASan/UBSan clean)"
else
    echo "FAIL: wsclient test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
