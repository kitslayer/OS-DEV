#!/bin/sh
# pve-deploy.sh — the reliable change->check loop for OS-DEV running as a
# first-class KVM guest on Proxmox.
#
# OS-DEV runs as a Proxmox VM ("osdev") on the real LAN: it takes a real DHCP
# lease and listens on the netcon debug console (tcp :2323), and `qm reset` gives
# out-of-band power control so even a hung build recovers (no watchdog needed).
# This script: rebuild -> upload the kernel -> hard-reset the VM -> read the boot
# log back over the network -> netcon-ping it to confirm it's alive.
#
# Config (override via env):
#   PVE_HOST   Proxmox node reached as ssh root@HOST   (default 192.168.1.5)
#   VMID       the osdev VM's id                        (default 122)
#   PVE_DIR    where the kernel/disk live on the node   (default /root/osdev)
#   CAP        seconds of serial to capture after reset (default 40)
#   SEND_DISK  1 = also re-upload fat.img (rarely needed; default 0)
#
# Usage:  tools/pve-deploy.sh            # build, deploy, reset, verify
#         VMID=130 tools/pve-deploy.sh   # target a different VM
set -e

PVE_HOST=${PVE_HOST:-192.168.1.5}
VMID=${VMID:-122}
PVE_DIR=${PVE_DIR:-/root/osdev}
CAP=${CAP:-40}
SEND_DISK=${SEND_DISK:-0}
SSH="ssh -o BatchMode=yes root@$PVE_HOST"

cd "$(dirname "$0")/.."

echo "==> building kernel (make)..."
make >/dev/null

echo "==> uploading kernel32.elf -> $PVE_HOST:$PVE_DIR ..."
scp -o BatchMode=yes build/kernel32.elf "root@$PVE_HOST:$PVE_DIR/kernel32.elf"
[ "$SEND_DISK" = 1 ] && scp -o BatchMode=yes build/fat.img "root@$PVE_HOST:$PVE_DIR/fat.img"

echo "==> hard-resetting VM $VMID and capturing ${CAP}s of serial..."
$SSH 'bash -s' "$VMID" "$CAP" <<'REMOTE'
VMID="$1"; CAP="$2"; LOG=/root/osdev/boot.log
qm reset "$VMID" >/dev/null 2>&1 || qm start "$VMID" >/dev/null 2>&1
sleep 1
timeout "$CAP" socat -u "UNIX-CONNECT:/var/run/qemu-server/$VMID.serial0" "OPEN:$LOG,creat,trunc" 2>/dev/null || true

echo "----- boot log (last 30 non-blank lines) -----"
tr -d '\r' < "$LOG" | grep -vE '^[[:space:]]*$' | tail -30

IP=$(tr -d '\r' < "$LOG" | grep -oE '192\.168\.[0-9]+\.[0-9]+' | tail -1)
if [ -n "$IP" ]; then
    echo "----- netcon liveness check ($IP:2323) -----"
    timeout 8 bash -c "exec 3<>/dev/tcp/$IP/2323 || exit 1; printf 'uptime\ncpu\n' >&3; sleep 3; timeout 2 cat <&3" 2>/dev/null \
        | tr -d '\r' | grep -vE '^[[:space:]]*$' | head -6 || echo "(netcon not reachable yet)"
else
    echo "(no LAN IP seen in the boot log — is the VM on netcon with DHCP?)"
fi
REMOTE

echo "==> done."
