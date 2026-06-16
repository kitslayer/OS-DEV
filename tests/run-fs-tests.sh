#!/bin/sh
# Build the from-scratch FAT32 read path for the host with ASan+UBSan and fuzz it
# against corrupt/cyclic on-disk structures. fs_test.c #includes fat32.c (to reach
# the static walk_dir / fat32_read / cluster_in_range / fat_step) and stubs the
# disk (ata_read/ata_write -> an in-memory image) + vfs_register. Locks the
# disk-trust-boundary robustness from M435 (cluster_in_range) + the cluster-chain
# cycle guard + the dir-recursion depth caps. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host FAT32 read path (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/fs/fs_test.c -o /tmp/osdev_fs_test
echo "running FAT32 corrupt-image fuzz..."
if /tmp/osdev_fs_test; then
    echo "PASS: fat32.c read path (ASan/UBSan clean on corrupt-FAT/dir fuzz)"
else
    echo "FAIL: fat32.c test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
