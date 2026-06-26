#!/bin/sh
# Build the reader-mode content extractor (kernel/reader.c) on the host with
# ASan+UBSan and run its regression + fuzz test. reader_main_region scans untrusted
# page HTML for the article container; an over-read while walking malformed tags is
# silent guard-page-less corruption in the kernel, so it's unit-tested off-target
# like the codebase's other extracted parsers (color/cssprop/cssel/htmlattr).
# It depends on htmlattr.c (find_attr), so both are compiled in. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host reader-mode extractor (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/reader/reader_test.c kernel/reader.c kernel/htmlattr.c -o /tmp/osdev_reader_test
echo "running reader_main_region regression + fuzz..."
if /tmp/osdev_reader_test; then
    echo "PASS: reader.c reader_main_region (article extraction; ASan/UBSan clean on malformed/truncated HTML fuzz)"
else
    echo "FAIL: reader.c test aborted (extraction contract mismatch or ASan/UBSan memory error)"; exit 1
fi
