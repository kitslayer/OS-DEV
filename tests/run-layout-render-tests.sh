#!/bin/sh
# In-guest regression for the browser's SOLVED box-model geometry (M1902).
#
# The layout RULES are unit-tested off-target (tests/layout), but the browser-side
# wiring had no automated coverage — which is how a background painting the
# content box (M1897), vertical padding landing outside the background (M1900),
# and a border stroking inside its own background (M1901) all coexisted in a
# fully-green tree. Each was caught by a human looking at a screenshot.
#
# This boots the desktop headlessly, opens LAYCHK.HTM (every case in a unique
# background colour), dumps the framebuffer, and asserts the geometry by finding
# each colour's bounding box. Assertions are relative, so window placement and
# desktop chrome can change without breaking them.
#
# SKIPs cleanly if QEMU/python3 are absent. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
if ! command -v "$QEMU" >/dev/null 2>&1; then echo "SKIP: layout-render test ($QEMU not found)"; exit 0; fi
if ! command -v python3 >/dev/null 2>&1; then echo "SKIP: layout-render test (python3 not found)"; exit 0; fi

OUT=$(mktemp -d /tmp/osdev_layrender.XXXXXX)
cleanup() { rc=$?; rm -rf "$OUT"; exit "$rc"; }
trap cleanup EXIT

# Capture, then check. Retried once: under host load the desktop can still be
# settling when the keys are injected, so the address bar never receives the URL
# and the dump shows no fixture at all. That is a harness-timing miss, not a
# geometry regression -- and it is distinguishable, because a miss loses EVERY
# colour while a real regression keeps the boxes and moves them. Only a
# no-colours-found result is retried.
capture() {
    rm -f "$OUT/laychk.ppm"
    echo "booting the desktop headlessly and opening LAYCHK.HTM (attempt $1)..."
    # Apps menu (F9) -> Enter opens the Browser -> '/' focuses the address bar.
    python3 tools/osdrive.py --out "$OUT" --boot-timeout 60 -c '
sleep 4;
key f9;
sleep 1.5;
key ret;
sleep 4;
key slash;
sleep 1.5;
type file:LAYCHK.HTM;
sleep 1.5;
key ret;
sleep 6;
shot laychk.ppm' >/dev/null 2>&1 || true
    [ -f "$OUT/laychk.ppm" ]
}

attempt=1
while : ; do
    if capture "$attempt"; then
        echo "checking solved box-model geometry..."
        out=$(python3 tests/layoutrender/check_geometry.py "$OUT/laychk.ppm" 2>&1) && rc=0 || rc=$?
        printf '%s\n' "$out"
        # A total miss (no colours at all) is a capture problem; retry once.
        if [ "$rc" != 0 ] && [ "$attempt" -lt 2 ] && printf '%s' "$out" | grep -q "did not render at all"; then
            echo "  (nothing rendered — retrying the capture once)"
            attempt=$((attempt+1)); continue
        fi
    else
        rc=1
        if [ "$attempt" -lt 2 ]; then
            echo "  (no dump produced — retrying the capture once)"
            attempt=$((attempt+1)); continue
        fi
        echo "FAIL: no framebuffer dump produced (boot or navigation failed)"
    fi
    break
done

if [ "$rc" = 0 ]; then
    echo "PASS: browser box-model geometry (§10.3.3 used widths incl. padding, auto-margin centring + left/right alignment, nested background inset, border encloses the padding box)"
else
    echo "FAIL: browser box-model geometry regressed"; exit 1
fi
