#!/bin/sh
# Build the from-scratch HTTP response parsers (extracted into kernel/http.c) for
# the host with ASan+UBSan and run the regression + fuzz test. These read raw
# bytes from untrusted servers/CDNs — the chunked-transfer decoder memmoves with
# attacker-controlled hex sizes — so a malformed/truncated response must never
# out-of-bounds or grow the buffer. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host HTTP parsers (ASan+UBSan)..."
$CC -std=gnu11 -O1 -g $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/http/http_test.c -o /tmp/osdev_http_test
echo "running HTTP-parser regression + fuzz..."
if /tmp/osdev_http_test; then
    echo "PASS: HTTP response parsers (ASan/UBSan clean on regression + chunk/header fuzz)"
else
    echo "FAIL: HTTP-parser test aborted (ASan/UBSan caught a memory error or a check failed)"; exit 1
fi
