#!/bin/sh
# Build the from-scratch TCP/IP packet parser + reassembly for the host with
# ASan+UBSan and fuzz them against adversarial packets. net_test.c #includes
# net.c (to reach the static tcp_recv_seg / ooo_store) and stubs the NIC+timer.
# Locks the bounds-safety verified in the M419-423 security audit. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo "building host TCP/IP parser (ASan+UBSan)..."
$CC -std=gnu11 -O1 $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/net/net_test.c -o /tmp/osdev_net_test
echo "running TCP/IP parser + reassembly fuzz..."
if /tmp/osdev_net_test; then
    echo "PASS: net.c parser + reassembly (ASan/UBSan clean on packet + ooo_store fuzz)"
else
    echo "FAIL: net.c test aborted (ASan/UBSan caught a memory error)"; exit 1
fi
