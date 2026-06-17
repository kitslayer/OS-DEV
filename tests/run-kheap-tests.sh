#!/bin/sh
# Build the from-scratch kernel heap (kmalloc/kfree/kzalloc) for the host with
# ASan+UBSan and torture it. The heap underlies every kernel allocation, so a
# split/coalesce/grow bug corrupts arbitrary kernel state. The test runs against
# a real mmap'd arena (kheap.c keeps a settable base for this) and verifies a
# per-block pattern + the free-list tiling invariant across 400k random ops.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host kernel heap (ASan+UBSan)..."
$CC -std=gnu11 -O1 -g $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/kheap/kheap_test.c -o /tmp/osdev_kheap_test
echo "running kernel-heap torture..."
if /tmp/osdev_kheap_test; then
    echo "PASS: kernel heap (ASan/UBSan clean on split/coalesce/grow torture)"
else
    echo "FAIL: kernel-heap test aborted (ASan/UBSan caught a memory error or a check failed)"; exit 1
fi
