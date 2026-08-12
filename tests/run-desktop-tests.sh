#!/bin/sh
# Headless assertion for the DESKTOP / window manager (M1925).
#
# Why this exists: the desktop had NO automated coverage at all. Window
# management, the keyboard chords and (as of M1923/M1924) virtual desktops were
# each verified once, by hand, by looking at a screenshot -- exactly the situation
# that let three box-model bugs coexist in a fully green tree until M1902. A
# regression here is silent: nothing else in `make check` opens a window.
#
# Method: drive the real desktop through the QEMU monitor with tools/osdrive.py,
# capture framebuffer dumps, and assert RELATIVE properties between them rather
# than absolute pixels, so the theme, wallpaper and window placement can all
# change freely without breaking this:
#
#   1. switching to an empty workspace must CHANGE the screen a lot (the windows
#      are gone), and
#   2. switching back must restore it EXACTLY -- byte-identical to the capture
#      taken before the switch. That is the strongest statement available about
#      workspace save/restore, and it is what makes this worth running: a
#      half-restored window set would differ by a few pixels and be caught.
#   3. Alt+F4 must actually close the focused window, which is observable as the
#      taskbar chip row getting shorter.
#
# SKIPs cleanly if QEMU or python3 is missing. Exit 0 = pass.
set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
command -v "$QEMU"   >/dev/null 2>&1 || { echo "SKIP: desktop test ($QEMU not found)"; exit 0; }
command -v python3   >/dev/null 2>&1 || { echo "SKIP: desktop test (python3 not found)"; exit 0; }
command -v socat     >/dev/null 2>&1 || { echo "SKIP: desktop test (socat not found)"; exit 0; }

OUT=$(mktemp -d /tmp/osdev_desk.XXXXXX)
cleanup() { rc=$?; rm -rf "$OUT"; exit "$rc"; }
trap cleanup EXIT

echo "booting the desktop headlessly and driving the window manager..."
# ws1 -> (switch) ws2 -> (switch back) ws1, then close a window on ws1.
python3 tools/osdrive.py --out "$OUT" --boot-timeout 90 -c '
sleep 6;
shot a_ws1.ppm;
key ctrl-alt-right;
sleep 2;
shot b_ws2.ppm;
key ctrl-alt-left;
sleep 2;
shot c_ws1_again.ppm;
key alt-f4;
sleep 2;
shot d_closed.ppm' >/dev/null 2>&1 || true

for f in a_ws1 b_ws2 c_ws1_again d_closed; do
    [ -f "$OUT/$f.ppm" ] || { echo "FAIL: no framebuffer dump '$f' (boot or drive failed)"; exit 1; }
done

python3 tests/desktop/check_wm.py "$OUT/a_ws1.ppm" "$OUT/b_ws2.ppm" \
                                  "$OUT/c_ws1_again.ppm" "$OUT/d_closed.ppm" && rc=0 || rc=$?
if [ "${rc:-1}" -eq 0 ]; then
    echo "PASS: desktop window manager (workspace switch hides windows, switching back restores them exactly, Alt+F4 closes)"
else
    echo "FAIL: desktop window manager"
    exit 1
fi
