#!/bin/sh
# In-guest HTTP SERVER test (M1304). The OS ships an `httpd` app (user/httpd.c,
# M1133) that LISTENS on TCP port 80 and serves a page via the from-scratch TCP
# server (net_tcp_serve) -- but it had no automated test. This boots the real
# kernel headless with a host->guest port forward (host:18080 -> guest:80), types
# `httpd` into the shell (which spawns the app), then curls the forwarded port
# FROM THE HOST and asserts the served page comes back. The host's curl output IS
# the assertion -- no guest-side serial marker needed. It proves the OS can accept
# an INBOUND TCP connection and serve a response end to end (passive open ->
# SYN-ACK -> read request -> send response -> close), the inbound counterpart of
# the outbound client the boot test already exercises.
#
# Exit 0 = pass. SKIPs cleanly if qemu/socat/curl are absent.
set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
KERNEL=build/kernel32.elf
DISK=build/fat.img
HPORT=18080                       # host port forwarded to the guest's httpd (:80)
TMP=$(mktemp -d /tmp/osdev_httpd.XXXXXX)
SOCK=$TMP/mon.sock
SLOG=$TMP/serial.log
QPID=""
cleanup() { rc=$?; [ -n "$QPID" ] && { kill -9 "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; }; rm -rf "$TMP"; exit "$rc"; }
trap cleanup EXIT

for t in "$QEMU" socat curl; do
    command -v "$t" >/dev/null 2>&1 || { echo "SKIP: httpd test ($t not found)"; exit 0; }
done

echo "booting kernel headless with a host->guest :$HPORT -> :80 forward..."
timeout -s KILL 70 "$QEMU" -no-reboot -no-shutdown -m 256M -kernel "$KERNEL" \
    -drive file="$DISK",format=raw,if=ide \
    -netdev user,id=net0,hostfwd=tcp::$HPORT-:80 -device e1000,netdev=net0 \
    -device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0 \
    -display none -serial file:"$SLOG" \
    -monitor unix:"$SOCK",server,nowait >"$TMP/qemu.err" 2>&1 &
QPID=$!

i=0; while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
[ -S "$SOCK" ] || { echo "FAIL: QEMU monitor socket never appeared"; cat "$TMP/qemu.err"; exit 1; }

got=0; i=0
while [ $i -lt 80 ]; do
    grep -q "launching the desktop" "$SLOG" 2>/dev/null && { got=1; break; }
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.5; i=$((i+1))
done
[ "$got" -eq 1 ] || { echo "FAIL: never reached desktop"; tail -6 "$SLOG"; exit 1; }
sleep 1.5

# Type `httpd<Enter>` into the focused Shell window (topmost at boot); the shell
# spawns the httpd app, which starts listening on TCP 80.
sendkey() { printf 'sendkey %s\n' "$1" | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true; sleep 0.25; }
for c in h t t p d; do sendkey "$c"; done
sendkey ret
sleep 2                           # let the app spawn + reach net_tcp_serve (listening)

# curl the forwarded port from the host, retrying to land inside a listen window
# (net_tcp_serve loops with ~3s windows; a missed SYN just retries).
out=""
for try in 1 2 3 4 5 6 7 8; do
    out=$(curl -s --max-time 4 "http://127.0.0.1:$HPORT/" 2>/dev/null || true)
    echo "$out" | grep -q "Hello from OS-DEV" && break
    sleep 1
done

if echo "$out" | grep -q "Hello from OS-DEV" && echo "$out" | grep -q "README.TXT"; then
    echo "PASS: in-guest httpd served its page + a LIVE file index over the from-scratch TCP stack (host curl received it)"
    echo "  served: $(echo "$out" | grep -o '<h1>[^<]*</h1>' | head -1) + a <pre> file listing (incl. README.TXT)"
    exit 0
else
    echo "FAIL: host curl did not receive the served page"
    echo "----- curl output -----"; echo "$out"
    echo "----- guest serial (tail) -----"; tail -10 "$SLOG" 2>/dev/null
    exit 1
fi
