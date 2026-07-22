#!/bin/sh
# Host-side deterministic proof of net.c's reliable TCP SENDER (M1886):
# retransmission on RTO, fast-retransmit on 3 duplicate ACKs, ACK processing /
# send-buffer freeing, and flow control against a small peer window — driven
# against a simulated peer over a LOSSY in-memory link (no QEMU). Under
# AddressSanitizer + UndefinedBehaviorSanitizer. tcp_reliable_test.c #includes
# net.c directly, so the exact kernel tcp_write/tcp_read are what's exercised.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
$CC -std=gnu11 -O1 -g $SAN -fno-stack-protector -Ikernel -Ikernel/include \
    tests/net/tcp_reliable_test.c kernel/url.c -o "$OUT/tcprel"

"$OUT/tcprel"
echo "tcpreliabletest: OK"
