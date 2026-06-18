#!/bin/sh
# Build the from-scratch inline-style scanner (kernel/cssprop.c) on the host with
# ASan+UBSan and fuzz style_prop over malformed/truncated/random style="" bytes.
# It runs kernel-side on untrusted page bytes (style attributes / <style> bodies),
# where an over-read is silent guard-page-less stack corruption. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host inline-style scanner (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/css/css_test.c -o /tmp/osdev_css_test
echo "running CSS style_prop regression + fuzz..."
if /tmp/osdev_css_test; then
    echo "PASS: cssprop.c style_prop (ASan/UBSan clean on malformed/truncated style fuzz)"
else
    echo "FAIL: cssprop.c test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
