#!/bin/sh
# Build the from-scratch ELF64 loader for the host with ASan+UBSan and run the
# regression + fuzz test. Confirms the loader validates untrusted ELF images
# (the ring-3 boundary) against the image size and the user address range so a
# malformed program can never out-of-bounds read or escape its range, and that
# a well-formed image still loads correctly. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host ELF loader (ASan+UBSan)..."
$CC -std=gnu11 -O1 -g $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/elf/elf_test.c -o /tmp/osdev_elf_test
echo "running ELF-loader regression + fuzz..."
if /tmp/osdev_elf_test; then
    echo "PASS: ELF loader (validators + load round-trip, fuzz/corrupt safe, ASan/UBSan clean)"
else
    echo "FAIL: ELF-loader test aborted (ASan/UBSan caught a memory error or a check failed)"; exit 1
fi
