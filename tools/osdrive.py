#!/usr/bin/env python3
"""osdrive.py -- drive OS-DEV headlessly under QEMU and capture the framebuffer.

QEMU emulates the VGA framebuffer even with -display none, and its monitor can
dump it (`screendump`) and inject input (keyboard over HMP, mouse over QMP).
This wraps all of that so you can script the desktop and screenshot it without a
display -- the workflow used to verify the M558-M561 desktop changes
(F1 help overlay, date clock, double-click maximize) entirely headlessly.

Usage:
    tools/osdrive.py [options] < script        # commands on stdin, one per line
    tools/osdrive.py [options] -c 'key f9; key ret; sleep 2; shot browser.png'
    echo 'shot desktop.png' | tools/osdrive.py  # just boot to desktop + screenshot

Options:
    --kernel PATH   multiboot kernel (default build/kernel32.elf)
    --disk PATH     FAT32 image    (default build/fat.img)
    --out DIR       where shots land (default .)
    --boot-timeout  seconds to wait for the desktop hand-off (default 25)
    --no-wait       don't wait for the desktop before running commands

Commands (one per line, or ';'-separated with -c):
    key NAME             HMP sendkey (f1 f9 ret esc up down left right a b ...)
    type TEXT            send each character of TEXT as a keystroke
    click X Y            left-click at screen pixel (X,Y)
    dblclick X Y         double-click at (X,Y)
    drag X0 Y0 X1 Y1     press at (X0,Y0), move to (X1,Y1), release
    move X Y             move the pointer to (X,Y) without clicking
    wheel X Y N          scroll the wheel at (X,Y): N>0 up, N<0 down
    mclick X Y           middle-click at (X,Y) (clipboard paste)
    rclick X Y           right-click at (X,Y) (copy browser link URL)
    sleep SECS           wait (float ok)
    shot FILE            screendump -> FILE (.png via PIL if available, else .ppm)
    wait-text STR        wait (up to boot-timeout) for STR to appear on COM1

The desktop's WM keys: f1 help, f2 switch, f3 min, f4 max, f5/f6 snap, f8 close,
f9 Apps menu, f12 screenshot-to-disk.
"""
import argparse, json, os, socket, subprocess, sys, tempfile, time, shutil

SCREEN_W, SCREEN_H = 1280, 960          # the desktop's mode (set by the Bochs VBE driver at boot)

# Map a literal character to its HMP `sendkey` name (so `type` can send URLs,
# paths, code, etc. — not just lowercase words). Returns None for unknown chars.
_UNSHIFTED = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "=": "equal",
              ";": "semicolon", "'": "apostrophe", ",": "comma", "`": "grave_accent",
              "[": "bracket_left", "]": "bracket_right", "\\": "backslash", "\t": "tab"}
_SHIFTED = {":": "semicolon", "?": "slash", "_": "minus", "+": "equal", "\"": "apostrophe",
            "(": "9", ")": "0", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5",
            "^": "6", "&": "7", "*": "8", "<": "comma", ">": "dot", "~": "grave_accent",
            "{": "bracket_left", "}": "bracket_right", "|": "backslash"}
def char_keyname(ch):
    if ch.islower() or ch.isdigit():       return ch
    if ch.isupper():                        return "shift-" + ch.lower()
    if ch in _UNSHIFTED:                    return _UNSHIFTED[ch]
    if ch in _SHIFTED:                      return "shift-" + _SHIFTED[ch]
    return None

class Mon:
    """A QEMU HMP monitor over a unix socket (text commands)."""
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path)
        self.s.settimeout(5)
        try: self.s.recv(4096)            # banner
        except OSError: pass
    def cmd(self, line):
        self.s.sendall((line + "\n").encode())
        time.sleep(0.05)
        try: self.s.recv(65536)
        except OSError: pass

class Qmp:
    """A QEMU QMP monitor over a unix socket (JSON; used for absolute mouse)."""
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path)
        self.f = self.s.makefile("rw")
        self.f.readline()                 # greeting
        self._cmd({"execute": "qmp_capabilities"})
    def _cmd(self, o):
        self.f.write(json.dumps(o) + "\n"); self.f.flush(); self.f.readline()
    def _ev(self, events):
        self._cmd({"execute": "input-send-event", "arguments": {"events": events}})
    def _abs(self, x, y):
        return [{"type": "abs", "data": {"axis": "x", "value": x * 32767 // SCREEN_W}},
                {"type": "abs", "data": {"axis": "y", "value": y * 32767 // SCREEN_H}}]
    def move(self, x, y):
        self._ev(self._abs(x, y)); time.sleep(0.06)
    def click(self, x, y, n=1):
        self._ev(self._abs(x, y)); time.sleep(0.06)
        for _ in range(n):
            self._ev([{"type": "btn", "data": {"down": True, "button": "left"}}])
            self._ev([{"type": "btn", "data": {"down": False, "button": "left"}}])
            time.sleep(0.05)
    def drag(self, x0, y0, x1, y1):
        self.move(x0, y0); time.sleep(0.08)
        self._ev([{"type": "btn", "data": {"down": True, "button": "left"}}]); time.sleep(0.08)
        for k in range(1, 11):
            self._ev(self._abs(x0 + (x1 - x0) * k // 10, y0 + (y1 - y0) * k // 10)); time.sleep(0.04)
        self._ev([{"type": "btn", "data": {"down": False, "button": "left"}}])
    def mclick(self, x, y):
        # middle-click at (x,y) — used for clipboard paste
        self._ev(self._abs(x, y)); time.sleep(0.06)
        self._ev([{"type": "btn", "data": {"down": True, "button": "middle"}}])
        self._ev([{"type": "btn", "data": {"down": False, "button": "middle"}}])
        time.sleep(0.05)
    def rclick(self, x, y):
        # right-click at (x,y) — e.g. copy a browser link's URL
        self._ev(self._abs(x, y)); time.sleep(0.06)
        self._ev([{"type": "btn", "data": {"down": True, "button": "right"}}])
        self._ev([{"type": "btn", "data": {"down": False, "button": "right"}}])
        time.sleep(0.05)
    def wheel(self, x, y, n):
        # scroll the wheel at (x,y): n>0 = up, n<0 = down
        self._ev(self._abs(x, y)); time.sleep(0.06)
        btn = "wheel-up" if n > 0 else "wheel-down"
        for _ in range(abs(n)):
            self._ev([{"type": "btn", "data": {"down": True, "button": btn}}])
            self._ev([{"type": "btn", "data": {"down": False, "button": btn}}])
            time.sleep(0.05)

def save_shot(mon, ppm_path, out_path):
    mon.cmd("screendump " + ppm_path); time.sleep(0.5)
    if out_path.endswith(".png"):
        try:
            from PIL import Image
            Image.open(ppm_path).save(out_path); return out_path
        except Exception:
            out_path = out_path[:-4] + ".ppm"
    shutil.copyfile(ppm_path, out_path); return out_path

def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--kernel", default="build/kernel32.elf")
    ap.add_argument("--disk",   default="build/fat.img")
    ap.add_argument("--disk2",  default=None, help="attach a 2nd drive (virtio-blk) — auto-mounted as /disk2")
    ap.add_argument("--disk3",  default=None, help="attach a 3rd drive (virtio-blk) — e.g. for a RAID mirror with --disk2")
    ap.add_argument("--hostfwd", default=None, help="QEMU user-net hostfwd spec, e.g. tcp::18080-:80 (inbound to the guest)")
    ap.add_argument("--cpu", default=None, help="QEMU -cpu model, e.g. max (to expose SMEP/UMIP etc.)")
    ap.add_argument("--out",    default=".")
    ap.add_argument("--boot-timeout", type=float, default=25)
    ap.add_argument("--no-wait", action="store_true")
    ap.add_argument("-c", "--commands", default=None)
    ap.add_argument("-h", "--help", action="store_true")
    args = ap.parse_args()
    if args.help:
        print(__doc__); return 0

    qemu = shutil.which("qemu-system-x86_64")
    if not qemu:
        print("osdrive: qemu-system-x86_64 not found", file=sys.stderr); return 2

    script = args.commands.replace(";", "\n") if args.commands else sys.stdin.read()
    cmds = [ln.strip() for ln in script.splitlines() if ln.strip() and not ln.strip().startswith("#")]

    tmp = tempfile.mkdtemp(prefix="osdrive.")
    mon_sock, qmp_sock, slog, ppm = (os.path.join(tmp, n) for n in
                                     ("mon.sock", "qmp.sock", "serial.log", "shot.ppm"))
    netdev = "user,id=net0" + (",hostfwd=" + args.hostfwd if args.hostfwd else "")
    qcmd = [qemu, "-no-reboot", "-no-shutdown", "-m", "256M", "-smp", "4", "-kernel", args.kernel,
        ] + (["-cpu", args.cpu] if args.cpu else []) + [
        "-drive", "file=%s,format=raw,if=ide" % args.disk,
        "-netdev", netdev, "-device", "e1000,netdev=net0",
        "-device", "piix3-usb-uhci,id=uhci", "-device", "usb-tablet,bus=uhci.0",
        "-device", "AC97,audiodev=snd0", "-audiodev", "none,id=snd0",
        "-display", "none", "-serial", "file:" + slog,
        "-monitor", "unix:%s,server,nowait" % mon_sock,
        "-qmp", "unix:%s,server,nowait" % qmp_sock]
    if args.disk2:
        qcmd += ["-drive", "file=%s,format=raw,if=none,id=d2,cache=writethrough" % args.disk2,
                 "-device", "virtio-blk-pci,drive=d2"]
    if args.disk3:   # attach via NVMe so it's a distinct non-boot writable disk (e.g. RAID member B)
        qcmd += ["-drive", "file=%s,format=raw,if=none,id=d3,cache=writethrough" % args.disk3,
                 "-device", "nvme,drive=d3,serial=osdev-d3"]
    qp = subprocess.Popen(qcmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rc = 0
    try:
        # wait for both control sockets
        deadline = time.time() + 10
        while (not os.path.exists(mon_sock) or not os.path.exists(qmp_sock)) and time.time() < deadline:
            time.sleep(0.1)
        mon, qmp = Mon(mon_sock), Qmp(qmp_sock)

        def wait_for(text, timeout):
            end = time.time() + timeout
            while time.time() < end:
                try:
                    if text in open(slog, "rb").read().decode("latin1"): return True
                except OSError: pass
                if qp.poll() is not None: return False
                time.sleep(0.3)
            return False

        if not args.no_wait:
            if not wait_for("launching the desktop", args.boot_timeout):
                print("osdrive: never reached the desktop", file=sys.stderr); rc = 1
            time.sleep(2)                 # let it paint

        for c in cmds:
            t = c.split()
            op = t[0]
            if   op == "key":      mon.cmd("sendkey " + t[1])
            elif op == "type":
                for ch in c[len("type"):].strip():
                    k = char_keyname(ch)
                    if k: mon.cmd("sendkey " + k)
            elif op == "click":    qmp.click(int(t[1]), int(t[2]))
            elif op == "dblclick": qmp.click(int(t[1]), int(t[2]), 2)
            elif op == "move":     qmp.move(int(t[1]), int(t[2]))
            elif op == "drag":     qmp.drag(int(t[1]), int(t[2]), int(t[3]), int(t[4]))
            elif op == "wheel":    qmp.wheel(int(t[1]), int(t[2]), int(t[3]))   # x y n (n>0 up, <0 down)
            elif op == "mclick":   qmp.mclick(int(t[1]), int(t[2]))             # middle-click (paste)
            elif op == "rclick":   qmp.rclick(int(t[1]), int(t[2]))             # right-click (copy link)
            elif op == "sleep":    time.sleep(float(t[1]))
            elif op == "wait-text":
                ok = wait_for(c[len("wait-text"):].strip(), args.boot_timeout)
                print("  wait-text:", "found" if ok else "TIMEOUT")
            elif op == "shot":
                out = save_shot(mon, ppm, os.path.join(args.out, t[1]))
                print("  shot ->", out)
            else:
                print("osdrive: unknown command:", c, file=sys.stderr); rc = 1
    finally:
        qp.kill(); qp.wait()
        try: shutil.copyfile(slog, os.path.join(args.out, "serial.log"))  # keep the boot log for debugging
        except OSError: pass
        shutil.rmtree(tmp, ignore_errors=True)
    return rc

if __name__ == "__main__":
    sys.exit(main())
