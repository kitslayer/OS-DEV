#!/bin/sh
# Build the from-scratch ext2 read path for the host with ASan+UBSan and fuzz it
# against corrupt on-disk metadata. ext2_test.c #includes ext2.c (device-agnostic
# via blk_read_fn) and serves an in-memory image. A real golden image is built
# with mke2fs (+ debugfs to add a small file and a >12KB file so the read path
# walks single/double-indirect blocks); the test then fuzzes corrupted copies of
# the superblock/group-descriptor/inode-table region. A corrupt superblock, bad
# block_size/inode-count, or a cyclic/huge indirect chain must never OOB or hang.
# Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
G=/tmp/osdev_ext2_golden.img

if ! command -v mke2fs >/dev/null 2>&1 || ! command -v debugfs >/dev/null 2>&1; then
    echo "SKIP: ext2test (mke2fs/debugfs not on host)"; exit 0
fi

echo "building golden ext2 image (mke2fs + a big file for indirect blocks)..."
rm -f "$G"
# plain ext2: no resize_inode/dir_index(HTree)/ext_attr/journal — matches what ext2.c reads
mke2fs -F -q -b 1024 -O ^resize_inode,^dir_index,^ext_attr,^has_journal "$G" 2048 >/dev/null 2>&1
printf 'hello ext2 fuzz harness\n' > /tmp/osdev_ext2_small.txt
head -c 300000 /dev/zero | tr '\0' 'A' > /tmp/osdev_ext2_big.txt    # 300 KB -> direct + single + double indirect
debugfs -w -R "write /tmp/osdev_ext2_small.txt HELLO" "$G" >/dev/null 2>&1
debugfs -w -R "write /tmp/osdev_ext2_big.txt BIG"      "$G" >/dev/null 2>&1

echo "building host ext2 read path (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/ext2/ext2_test.c -o /tmp/osdev_ext2_test

echo "running ext2 corrupt-image fuzz..."
if timeout 120 /tmp/osdev_ext2_test "$G"; then
    echo "PASS: ext2.c read path (ASan/UBSan clean on corrupt-metadata fuzz)"
else
    echo "FAIL: ext2.c test aborted (ASan/UBSan caught a memory error, or it hung)"; exit 1
fi
