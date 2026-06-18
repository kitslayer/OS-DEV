#!/bin/sh
# Build the from-scratch URL splitter/resolver (kernel/url.c) on the host with
# ASan+UBSan and fuzz url_split/resolve_img_url over malformed, truncated, and
# randomly-corrupted URLs with tiny output buffers. These run kernel-side on
# untrusted page bytes (the address bar, <a href>, <img src>, redirects), where
# an over-write is silent guard-page-less stack corruption. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host URL splitter/resolver (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/url/url_test.c -o /tmp/osdev_url_test
echo "running URL regression + fuzz..."
if /tmp/osdev_url_test; then
    echo "PASS: url.c splitter/resolver (ASan/UBSan clean on malformed/truncated URL fuzz)"
else
    echo "FAIL: url.c test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
