#!/bin/sh
# M1183: fuzz the from-scratch ISO 9660 read path (kernel/iso9660.c) on the host
# under ASan+UBSan. iso9660_test.c #includes iso9660.c (device-agnostic via
# blk_read_fn) and serves an in-memory disk; it hand-builds a minimal VALID ISO
# (no genisoimage needed), confirms probe/list/read, then fuzzes corrupted copies
# of the volume-descriptor + root-directory region. A corrupt PVD, bad root
# extent, overlong record length, or huge data length must never OOB or hang.
# Exit 0 = pass. (Matches the ext2 fuzz harness from M1180.)
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"

echo "building host ISO 9660 read path (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/iso9660/iso9660_test.c -o /tmp/osdev_iso9660_test

echo "running iso9660 corrupt-image fuzz..."
if timeout 120 /tmp/osdev_iso9660_test; then
    echo "PASS: iso9660.c read path (ASan/UBSan clean on corrupt-metadata fuzz)"
else
    echo "FAIL: iso9660.c test aborted (ASan/UBSan caught a memory error, or it hung)"; exit 1
fi
