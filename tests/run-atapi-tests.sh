#!/bin/sh
# In-guest assertion for the ATAPI CD-ROM read driver (kernel/atapi.c, M1852).
# ata.c is a plain-ATA disk driver that SKIPS ATAPI-signature devices; atapi.c
# claims them and reads 2048-byte logical sectors via the SCSI PACKET protocol
# (READ CAPACITY + READ(10)), all PIO/polled.
#
# Boots the real kernel headless with a CD-ROM (a minimal ISO 9660 image made
# here, whose only real content is the Primary Volume Descriptor at logical
# sector 16) attached via -cdrom, and captures COM1. The boot disk stays on
# legacy ATA; the CD is a separate device, so make check's default config is
# untouched. Asserts:
#   - the driver detected an ATAPI CD-ROM and read its capacity;
#   - it read logical sector 16 over the PACKET transport and found the ISO 9660
#     "CD001" magic (proves detect + PACKET + READ(10) at a non-zero LBA);
#   - the boot path stayed intact (reached the desktop) with no panic.
# Exit 0 = pass. SKIPs cleanly if QEMU or python3 is absent.
set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
KERNEL=build/kernel32.elf
DISK=build/fat.img
LOG=$(mktemp /tmp/osdev_atapi.XXXXXX.log)
ISO=$(mktemp /tmp/osdev_atapi.XXXXXX.iso)
QPID=""
cleanup() { rc=$?; [ -n "$QPID" ] && { kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; }; rm -f "$LOG" "$ISO"; exit "$rc"; }
trap cleanup EXIT

if ! command -v "$QEMU" >/dev/null 2>&1; then echo "SKIP: ATAPI test ($QEMU not found)"; exit 0; fi
if ! command -v python3 >/dev/null 2>&1; then echo "SKIP: ATAPI test (python3 not found to build the ISO)"; exit 0; fi

# Minimal ISO 9660: 18 logical sectors; sector 16 = Primary Volume Descriptor
# (type 1, "CD001", version 1), sector 17 = terminator (type 255). Enough for the
# driver to read + the "CD001" magic check to pass.
python3 - "$ISO" <<'PY'
import sys
d = bytearray(2048 * 18)
pvd = 16 * 2048
d[pvd] = 1; d[pvd+1:pvd+6] = b"CD001"; d[pvd+6] = 1
term = 17 * 2048
d[term] = 255; d[term+1:term+6] = b"CD001"; d[term+6] = 1
open(sys.argv[1], "wb").write(d)
PY

echo "booting kernel headless with an ATAPI CD-ROM attached (COM1 capture)..."
timeout -s KILL 40 "$QEMU" -no-reboot -no-shutdown -m 256M -kernel "$KERNEL" \
    -drive file="$DISK",format=raw,if=ide \
    -cdrom "$ISO" \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -display none -serial file:"$LOG" >/dev/null 2>&1 &
QPID=$!
i=0
while [ $i -lt 70 ]; do
    grep -q "launching the desktop" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.5; i=$((i+1))
done
sleep 0.3
kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; QPID=""

fail=0
require() {
    if grep -qiF "$1" "$LOG"; then echo "  ok: $2"
    else echo "  MISSING: $2  (expected substring: '$1')"; fail=1; fi
}
require "ATAPI CD-ROM"                        "detected the ATAPI CD-ROM + read its capacity"
require "read PVD ok, CD001 found"            "PACKET READ(10) of sector 16 returned the ISO 9660 PVD"
require "launching the desktop environment"   "reached desktop launch (boot path intact)"

forbid() {
    if grep -qiE "$1" "$LOG"; then echo "  CRASH MARKER: $2"; grep -inE "$1" "$LOG" | head -2 | sed 's/^/      /'; fail=1; fi
}
forbid "panic"        "kernel panic"
forbid "unhandled"    "unhandled exception"

if [ "$fail" = 0 ]; then echo "PASS: ATAPI CD-ROM driver (detect + PACKET READ(10) + ISO 9660 PVD, boot intact)"
else echo "FAIL: ATAPI CD-ROM test"; exit 1; fi
