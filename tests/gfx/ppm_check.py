#!/usr/bin/env python3
# Analyze a QEMU `screendump` PPM (P6) of the OS framebuffer and assert it shows
# a real, painted desktop -- not a blank screen, not the 640x480 text console,
# not an all-black hang. Pure stdlib (no PIL): the P6 format is a tiny ASCII
# header ("P6\n<w> <h>\n<max>\n") followed by w*h*3 raw RGB bytes.
#
# Usage: ppm_check.py <file.ppm> [min_width] [min_height] [min_colors]
# Exit 0 = looks like a painted desktop; 1 = does not (prints why).
import sys

def die(msg):
    print("  gfx-check: " + msg)
    sys.exit(1)

argv = sys.argv[1:]
# Optional: --white N requires at least N pure-white (>=246 on all channels)
# pixels -- used to assert a white-backgrounded page (e.g. the browser) painted,
# vs the dark desktop which has almost none.
min_white = 0
if argv and argv[0] == "--white":
    min_white = int(argv[1]); argv = argv[2:]

path = argv[0]
min_w = int(argv[1]) if len(argv) > 1 else 1024
min_h = int(argv[2]) if len(argv) > 2 else 768
min_colors = int(argv[3]) if len(argv) > 3 else 40

try:
    with open(path, "rb") as f:
        data = f.read()
except OSError as e:
    die("cannot read %s (%s)" % (path, e))

if not data.startswith(b"P6"):
    die("not a P6 PPM (got %r)" % data[:8])

# Parse the header: P6, width, height, maxval -- three whitespace-separated
# integers after the magic, tolerating comment lines (# ...).
fields, i, n = [], 2, len(data)
while len(fields) < 3 and i < n:
    while i < n and data[i:i+1].isspace():
        i += 1
    if data[i:i+1] == b"#":                      # comment to end of line
        while i < n and data[i:i+1] != b"\n":
            i += 1
        continue
    start = i
    while i < n and not data[i:i+1].isspace():
        i += 1
    fields.append(int(data[start:i]))
i += 1                                            # single whitespace after maxval
w, h, maxval = fields
pixels = data[i:]
if len(pixels) < w * h * 3:
    die("truncated pixel data: %d bytes, need %d for %dx%d" % (len(pixels), w*h*3, w, h))

if w < min_w or h < min_h:
    die("resolution %dx%d below desktop mode %dx%d (did the mode-set / desktop fail?)"
        % (w, h, min_w, min_h))

# Distinct colors over a stride-sampled set of pixels (full scan is needless).
# Also track the most common color's share to catch an all-one-color screen.
from collections import Counter
c = Counter()
stride = 3 * 7                                     # every 7th pixel
for off in range(0, w * h * 3, stride):
    c[pixels[off:off+3]] += 1
distinct = len(c)
total = sum(c.values())
top_color, top_n = c.most_common(1)[0]
top_share = top_n / total

print("  gfx-check: %dx%d, %d distinct colors (sampled), top color %s = %.1f%%"
      % (w, h, distinct, tuple(top_color), 100 * top_share))

if distinct < min_colors:
    die("only %d distinct colors (< %d): screen looks blank/unpainted" % (distinct, min_colors))
if top_share > 0.98:
    die("%.1f%% of the screen is one color: looks like an all-black hang" % (100 * top_share))

if min_white:
    white = 0
    for off in range(0, w * h * 3, 3):
        if pixels[off] >= 246 and pixels[off+1] >= 246 and pixels[off+2] >= 246:
            white += 1
    print("  gfx-check: %d pure-white pixels (need >= %d)" % (white, min_white))
    if white < min_white:
        die("only %d white pixels (< %d): the expected white-page content did not render" % (white, min_white))

print("  gfx-check: OK (painted desktop)")
sys.exit(0)
