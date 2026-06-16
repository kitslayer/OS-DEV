#!/bin/sh
# Build the ustar tar extractor (kernel/tar.c) for the host with ASan+UBSan,
# generate a known archive with python tarfile, and verify exact extraction +
# corrupt-input fuzz. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
python3 - <<'PY'
import tarfile, io
t = tarfile.open('/tmp/osdev_t.tar', 'w', format=tarfile.USTAR_FORMAT)
def add(name, data):
    ti = tarfile.TarInfo(name); ti.size = len(data); t.addfile(ti, io.BytesIO(data))
add('hello.txt', b'hello tar\n')
add('big.txt', b'X' * 5000)
add('sub/y.txt', b'nested tar file')
t.close()
PY
echo "building host tar extractor (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -Ikernel/include tests/tar/tar_test.c kernel/tar.c -o /tmp/osdev_tar_test
echo "running tar extractor round-trip + fuzz..."
if /tmp/osdev_tar_test; then
    echo "PASS: tar extractor (exact extraction, fuzz/corrupt safe, ASan/UBSan clean)"
else
    echo "FAIL: tar extractor test aborted"; exit 1
fi
