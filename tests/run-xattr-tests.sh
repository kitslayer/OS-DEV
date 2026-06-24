#!/bin/sh
# M1182: prove the from-scratch ext2 extended-attribute WRITE path is correct AND
# interoperable with the real Linux ext2 tools. xattr_test.c #includes ext2.c and
# round-trips user.* xattrs (set/get/list/replace/remove) on a real mke2fs image
# under ASan+UBSan; then this runner validates the resulting on-disk image with
# e2fsck (must stay CLEAN — the bytes we wrote are spec-correct) and reads the
# xattr back with debugfs (the reference tool sees what we wrote). Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
IN=/tmp/osdev_xattr_in.img
OUT=/tmp/osdev_xattr_out.img

if ! command -v mke2fs >/dev/null 2>&1 || ! command -v debugfs >/dev/null 2>&1 || ! command -v e2fsck >/dev/null 2>&1; then
    echo "SKIP: xattrtest (mke2fs/debugfs/e2fsck not on host)"; exit 0
fi

echo "building ext2 image (256-byte inodes + ext_attr) with a target file..."
rm -f "$IN" "$OUT"
mke2fs -F -q -b 1024 -I 256 -O ext_attr,^resize_inode,^dir_index,^has_journal "$IN" 2048 >/dev/null 2>&1
printf 'the quick brown fox\n' > /tmp/osdev_xattr_F.txt
debugfs -w -R "write /tmp/osdev_xattr_F.txt F.TXT" "$IN" >/dev/null 2>&1

echo "building + running the xattr round-trip (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/ext2/xattr_test.c -o /tmp/osdev_xattr_test
if ! /tmp/osdev_xattr_test "$IN" "$OUT"; then
    echo "FAIL: xattr round-trip (a check failed, or ASan/UBSan caught a memory error)"; exit 1
fi

echo "validating the on-disk image with the real ext2 tools..."
# e2fsck must find nothing to fix (our byte layout is spec-clean).
if ! e2fsck -fn "$OUT" >/tmp/osdev_xattr_fsck.log 2>&1; then
    echo "FAIL: e2fsck found errors in our xattr image:"; cat /tmp/osdev_xattr_fsck.log; exit 1
fi
# debugfs (the reference tool) must read the xattr we wrote.
EA=$(debugfs -R "ea_get /F.TXT user.greeting" "$OUT" 2>/dev/null | tr -d '\n')
case "$EA" in
    *hi*) : ;;  # value is "hi" after the replace+remove sequence
    *) echo "FAIL: debugfs did not read back user.greeting=hi (got: '$EA')"; exit 1 ;;
esac

echo "PASS: ext2 xattr write path (round-trip OK; e2fsck clean; debugfs reads it: user.greeting=$EA)"
