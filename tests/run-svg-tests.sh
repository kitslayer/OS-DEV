#!/bin/sh
# Build the from-scratch SVG rasterizer for the host with ASan+UBSan and fuzz it
# against adversarial/truncated/malformed SVG XML. svg_test.c compiles kernel/svg.c
# directly and checks unit cases (rect/circle/path-bezier render correctly) plus a
# large random+mutation+structured fuzz. svg.c parses untrusted web XML in-kernel,
# so this locks its bounds-safety (no OOB/overflow/hang on hostile input). Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host SVG rasterizer (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/svg/svg_test.c kernel/svg.c -o /tmp/osdev_svg_test
echo "running SVG unit + fuzz tests..."
if /tmp/osdev_svg_test; then
    echo "PASS: svg.c rasterizer (ASan/UBSan clean on unit cases + adversarial fuzz)"
else
    echo "FAIL: svg.c test aborted (ASan/UBSan caught a memory error or a unit case failed)"; exit 1
fi
