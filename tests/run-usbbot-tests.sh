#!/bin/sh
# Build the shared USB Mass-Storage Bulk-Only-Transport + SCSI layer
# (kernel/usbbot.h) for the host with ASan+UBSan and run its regression against
# a mock BOT device. Pure, so unit-testable off-target like the other extracted
# protocol layers. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'building USB BOT/SCSI layer (ASan+UBSan)...'
$CC -std=gnu11 -O1 $SAN -Wall -Wextra -Ikernel tests/usbbot/usbbot_test.c -o /tmp/osdev_usbbot_test
echo "running BOT/SCSI regression against a mock device..."
if /tmp/osdev_usbbot_test; then
    echo "PASS: USB BOT/SCSI (CBW/CSW wire format, tags, residue trust rules, READ/WRITE(10) round-trip, chunking, bounds, fault injection, ASan/UBSan clean)"
else
    echo "FAIL: usbbot test aborted (semantics mismatch or ASan/UBSan memory error)"; exit 1
fi
