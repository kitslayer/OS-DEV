#!/bin/sh
# Headless assertion for the POSIX IPC surface (M1906): message queues, named
# semaphores, shared memory, ptys, advisory file locks, inotify and eventfd.
#
# A coverage survey found ~2,700 lines across these subsystems with NO automated
# assertions at all. They are reachable from userspace, so they were exercised
# incidentally, but nothing checked their semantics — the same situation that let
# three box-model bugs coexist in a green tree until M1902.
#
# The userspace test apps (user/iouringtest.c etc.) cannot be used for this: ring-3
# print() goes to an app's window text grid and is NOT mirrored to COM1, so a
# headless run cannot read it. Kernel kprintf DOES reach COM1, so the assertions
# live in kernel/ipcselftest.c and this script greps the markers — the same shape
# as the driver suites (ahcitest, nvmetest, ...).
#
# SKIPs cleanly if QEMU is absent. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
KERNEL=build/kernel32.elf
DISK=build/fat.img
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: ipc test ($QEMU not found)"; exit 0; }

SLOG=$(mktemp /tmp/osdev_ipc.XXXXXX.log)
QPID=""
cleanup() { rc=$?; [ -n "$QPID" ] && { kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; }; rm -f "$SLOG"; exit "$rc"; }
trap cleanup EXIT

echo "booting kernel headless and running the POSIX IPC self-test (COM1 capture)..."
timeout -s KILL 90 "$QEMU" -no-reboot -no-shutdown -m 256M -kernel "$KERNEL" \
    -drive file="$DISK",format=raw,if=ide \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0 \
    -device AC97,audiodev=snd0 -audiodev none,id=snd0 \
    -display none -serial file:"$SLOG" >/dev/null 2>&1 &
QPID=$!
i=0
while [ $i -lt 160 ]; do
    grep -q "ipc self-test:" "$SLOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.5; i=$((i+1))
done
sleep 0.3
kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; QPID=""

if ! grep -q "ipc self-test:" "$SLOG" 2>/dev/null; then
    echo "FAIL: the IPC self-test never ran (boot problem, not an IPC regression)"
    tail -12 "$SLOG" 2>/dev/null | sed 's/^/      /'
    exit 1
fi

fail=0
require() {
    if grep -qF "$1" "$SLOG"; then echo "  ok: $2"
    else echo "  MISSING: $2  (expected substring: '$1')"; fail=1; fi
}

# Spot-check the semantics that a regression would actually break, rather than
# just that the suite ran. Each of these is a property, not a smoke test.
require "mq_receive returned the HIGHEST priority message first"  "mqueue is priority-ordered, not FIFO"
require "mq_unlink removed the queue"                             "mqueue unlink"
require "sem_open without O_CREAT fails for a missing name"       "sem_open honours O_CREAT (ENOENT without it)"
require "sem_open O_CREAT|O_EXCL fails on an existing name"       "sem_open honours O_EXCL (EEXIST)"
require "sem_trywait fails at 0 instead of blocking"              "sem_trywait is non-blocking at zero"
require "shm_open of the same name returned the SAME backing frames" "shm actually shares its frames"
require "shm_open past the size cap is rejected"                  "shm enforces its size cap"
require "pty_read on the slave got those bytes"                   "pty master->slave data path"
require "flock LOCK_EX refused to pid 102 while 101 holds it"     "flock exclusive locks exclude"
require "flock LOCK_SH ALSO granted to pid 102"                   "flock shared locks coexist"
require "flock LOCK_EX granted after the holders' pids were released" "flock releases a dead pid's locks"
require "inotify saw an event after a matching VFS mutation"      "inotify observes real VFS mutations"
require "inotify ignores mutations outside its watch"             "inotify does not over-report"
require "eventfd_read returned the accumulated count"             "eventfd accumulates and drains"

# And the summary must report zero failures.
if grep -qE "ipc self-test: [0-9]+ passed, 0 failed" "$SLOG"; then
    n=$(grep -oE "ipc self-test: [0-9]+ passed" "$SLOG" | grep -oE "[0-9]+" | head -1)
    echo "  ok: all $n IPC assertions passed in-guest"
else
    echo "  FAIL: the self-test reported failures:"
    grep -E "^\[FAIL\] ipc|ipc self-test:" "$SLOG" | sed 's/^/      /'
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: in-guest POSIX IPC (mqueue priority order, sem O_CREAT/O_EXCL + non-blocking trywait, shm frame sharing + size cap, pty data path, flock exclusion/sharing/pid-release, inotify filtering, eventfd accumulate+drain)"
else
    echo "FAIL: in-guest POSIX IPC self-test"; exit 1
fi
