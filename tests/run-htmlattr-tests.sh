#!/bin/sh
# Build the from-scratch HTML attribute scanners (kernel/htmlattr.c) on the host
# with ASan+UBSan and fuzz find_attr/has_attr/attr_int/find_href over malformed,
# truncated, and randomly-corrupted attribute slices. These run kernel-side on
# untrusted page bytes (a hostile web server's tags), where an over-read is
# silent guard-page-less stack corruption. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host HTML attribute scanners (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/htmlattr/htmlattr_test.c -o /tmp/osdev_htmlattr_test
echo "running HTML-attribute regression + fuzz..."
if /tmp/osdev_htmlattr_test; then
    echo "PASS: htmlattr.c scanners (ASan/UBSan clean on malformed/truncated attribute fuzz)"
else
    echo "FAIL: htmlattr.c test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
