#!/bin/sh
# In-guest boot smoke assertion. Boots the real kernel headless under QEMU
# (COM1 -> a log), then asserts every critical bring-up marker is present and
# that no fault/panic occurred. Unlike the host suites (which #include one .c
# in isolation), this exercises the whole kernel + driver stack end to end:
# preemption, address-space isolation, PCI enumeration, the e1000/IP/TCP stack
# (ARP + ICMP + a real HTTP GET over SLIRP), the FAT32 driver, the AC'97 audio
# bring-up, and the USB tablet. Exit 0 = pass.
#
# QEMU was unavailable for many milestones (a SIGSTKFLT launch failure), which
# is why so much landed host-verified only; this guard makes the boot a gated
# regression again now that it runs.
set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
KERNEL=build/kernel32.elf
DISK=build/fat.img
LOG=$(mktemp /tmp/osdev_boot.XXXXXX.log)
QPID=""
cleanup() { rc=$?; [ -n "$QPID" ] && { kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; }; rm -f "$LOG"; exit "$rc"; }
trap cleanup EXIT

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "SKIP: boot test ($QEMU not found)"
    exit 0
fi

echo "booting kernel headless under QEMU (COM1 capture)..."
# Capture COM1 to a file and poll it: kill QEMU as soon as the boot finishes
# (the "launching the desktop" hand-off is the last marker) rather than always
# burning a fixed cap. The 25s timeout is a generous safety net -- the boot does
# a real TLS 1.3 HTTPS handshake (bignum-heavy under TCG) before the desktop, so
# a tight cap would be flaky. SIGKILL because -no-shutdown ignores SIGTERM.
timeout -s KILL 25 "$QEMU" -no-reboot -no-shutdown -m 256M -kernel "$KERNEL" \
    -drive file="$DISK",format=raw,if=ide \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0 \
    -device AC97,audiodev=snd0 -audiodev none,id=snd0 \
    -display none -serial file:"$LOG" >/dev/null 2>&1 &
QPID=$!
i=0
while [ $i -lt 50 ]; do
    grep -q "launching the desktop" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break    # QEMU exited (crash, or a stubbed binary): stop waiting
    sleep 0.5; i=$((i+1))
done
sleep 0.3   # let the last few lines flush
kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; QPID=""

fail=0

# Markers that MUST appear, in the order the kernel prints them. Each is a
# distinct subsystem coming up; a missing one means that subsystem regressed.
require() {
    if grep -qiF "$1" "$LOG"; then
        echo "  ok: $2"
    else
        echo "  MISSING: $2  (expected substring: '$1')"
        fail=1
    fi
}
# Soft markers: depend on the host actually having outbound internet (the kernel
# GETs the real example.com). Reported but NOT fatal, so the gate stays green on
# an offline machine -- the SLIRP gateway ping below covers the stack regardless.
softrequire() {
    if grep -qiF "$1" "$LOG"; then echo "  ok: $2"
    else echo "  (skip: $2 -- no marker '$1'; offline host? not fatal)"; fi
}
require "full bring-up complete"             "core bring-up (PMM/VMM/IDT)"
require "preemption works"                   "preemptive scheduler"
require "each process has its own address"   "per-process address-space isolation"
require "PCI devices on the bus"             "PCI enumeration"
require "AML method evaluation OK"           "ACPI AML method-evaluation VM (recursion/If + While, M1289)"
require "Networking works!"                  "e1000 + ARP + ICMP echo (SLIRP gateway)"
softrequire "200 OK"                         "TCP/HTTP GET to real example.com (needs internet)"
softrequire "certverify=ok"                  "TLS 1.3 HTTPS to example.com: chain validated + certverify (needs internet)"
require "mounted FAT32 volume"               "FAT32 mount"
require "AC'97 audio: NAM="                  "AC'97 audio bring-up"
require "USB tablet active"                  "USB UHCI + tablet"
require "launching the desktop environment"  "reached desktop launch"

# Markers that must NOT appear: a crash anywhere in the boot.
forbid() {
    if grep -qiE "$1" "$LOG"; then
        echo "  CRASH MARKER: $2"
        grep -inE "$1" "$LOG" | head -3 | sed 's/^/      /'
        fail=1
    fi
}
forbid "panic"                       "kernel panic"
forbid "unhandled (interrupt|excep)" "unhandled exception"
forbid "page fault"                  "page fault"
forbid "general protection"          "#GP fault"

if [ "$fail" -eq 0 ]; then
    echo "PASS: in-guest boot (10 required bring-up markers present, no crash)"
    exit 0
else
    echo "FAIL: in-guest boot smoke test"
    echo "----- captured COM1 log -----"
    cat "$LOG"
    exit 1
fi
