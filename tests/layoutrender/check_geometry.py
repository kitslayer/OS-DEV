#!/usr/bin/env python3
"""check_geometry.py — assert the browser's SOLVED box-model geometry from a
framebuffer dump (M1902).

Why this exists: the layout rules themselves are unit-tested off-target
(tests/layout, 95 known-answer checks), but the browser-side wiring had no
automated coverage at all. Three real bugs coexisted in a fully-green tree
because of it — a background painting the content box instead of the padding box
(M1897), vertical padding landing outside the background (M1900), and a border
stroking inside its own background (M1901). Each was found by a human looking at
a screenshot. This closes that gap.

Method: LAYCHK.HTM gives every case a unique background colour, so each block's
box is findable by exact RGB match. Assertions are RELATIVE (widths, and gaps
compared against each other) rather than absolute screen coordinates, so they
survive window placement and desktop chrome changes. The full-width padded block
establishes the content column that the alignment cases are measured against.

Usage: check_geometry.py <dump.ppm>   ->  exit 0 = pass
"""
import sys

TOL = 3          # px slack for integer rounding / odd-slack splits

# Colours must match tools/mkfatfs.c's LAYCHK.HTM exactly.
COL = {
    "wide_bg":   (0xfe, 0x06, 0x06),   # padding:40px, no margins -> spans the column
    "wide_in":   (0xfe, 0x07, 0x07),   # a nested <p> background inside it
    "left":      (0xfe, 0x01, 0x01),   # width:300px
    "centre":    (0xfe, 0x02, 0x02),   # width:300px; margin:0 auto
    "right":     (0xfe, 0x03, 0x03),   # width:300px; margin-left:auto
    "padbox":    (0xfe, 0x04, 0x04),   # width:400px; padding:20px; margin:0 auto
    "bordered":  (0xfe, 0x05, 0x05),   # border:3px + padding:30px
    "border":    (0x01, 0xfe, 0x01),   # that block's border stroke
    "bordbox":   (0xfe, 0x08, 0x08),   # width:400px; padding:20px; box-sizing:border-box
    "mb":        (0xfe, 0x09, 0x09),   # width:200px; margin-bottom:60px
    "after_mb":  (0xfe, 0x0a, 0x0a),   # the block that follows it
}


def read_ppm(path):
    """Minimal P6 reader, tolerant of comments and whitespace layout."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("FAIL: %s is not a binary PPM (P6)" % path)
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":                      # comment to end of line
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    w, h, maxv = fields
    if maxv != 255:
        raise SystemExit("FAIL: unsupported PPM maxval %d" % maxv)
    return w, h, data[i + 1:]


def bbox(px, w, h, rgb):
    """Bounding box of pixels exactly matching rgb, or None. Returns
    (left, top, right, bottom) with right/bottom INCLUSIVE."""
    target = bytes(rgb)
    l, t, r, b = w, h, -1, -1
    for y in range(h):
        row = px[y * w * 3:(y + 1) * w * 3]
        start = row.find(target)
        if start < 0:
            continue
        # scan the row properly: find() can land on a mid-pixel byte alignment
        for x in range(w):
            if row[x * 3:x * 3 + 3] == target:
                if x < l: l = x
                if x > r: r = x
                if y < t: t = y
                if y > b: b = y
    return None if r < 0 else (l, t, r, b)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_geometry.py <dump.ppm>")
    w, h, px = read_ppm(sys.argv[1])

    boxes, missing = {}, []
    for name, rgb in COL.items():
        bb = bbox(px, w, h, rgb)
        if bb is None:
            missing.append(name)
        boxes[name] = bb
    if missing:
        print("FAIL: these cases did not render at all: %s" % ", ".join(sorted(missing)))
        print("      (the page may not have loaded, or a colour changed in LAYCHK.HTM)")
        return 1

    fails, checks = [], 0

    def ck(cond, msg):
        nonlocal checks
        checks += 1
        if cond:
            print("  ok: %s" % msg)
        else:
            print("  FAIL: %s" % msg)
            fails.append(msg)

    def width(n):
        l, _, r, _ = boxes[n]
        return r - l + 1

    # The full-width padded block has no margins, so its padding box spans the
    # whole content column — that is our reference frame.
    col_l, _, col_r, _ = boxes["wide_bg"]

    # --- §10.3.3 used width -------------------------------------------------
    for name in ("left", "centre", "right"):
        ck(abs(width(name) - 300) <= TOL,
           "%s: used width is 300px (got %d)" % (name, width(name)))
    # padding is part of the width constraint: 400 content + 2*20 padding
    ck(abs(width("padbox") - 440) <= TOL,
       "padbox: width:400px + padding:20px gives a 440px box (got %d)" % width("padbox"))

    # --- horizontal alignment ------------------------------------------------
    ck(abs(boxes["left"][0] - col_l) <= TOL,
       "left: no auto margin stays at the column's left edge")
    ck(abs(boxes["right"][2] - col_r) <= TOL,
       "right: margin-left:auto pushes the box to the column's right edge")
    for name in ("centre", "padbox"):
        gl = boxes[name][0] - col_l
        gr = col_r - boxes[name][2]
        ck(abs(gl - gr) <= TOL,
           "%s: margin:0 auto centres it (gaps %d vs %d)" % (name, gl, gr))

    # --- box-sizing:border-box (M1903) --------------------------------------
    # With border-box the specified width INCLUDES the padding, so the box is
    # exactly 400px wide -- not 440px as the same declaration gives under the
    # default content-box (asserted above as `padbox`).
    ck(abs(width("bordbox") - 400) <= TOL,
       "border-box: width:400px + padding:20px stays a 400px box (got %d)" % width("bordbox"))
    ck(width("padbox") - width("bordbox") >= 2 * 20 - TOL,
       "border-box differs from content-box by the padding (%d vs %d)"
       % (width("padbox"), width("bordbox")))

    # --- margin-bottom (M1903) ----------------------------------------------
    # It was previously dropped entirely. It must push the FOLLOWING block down,
    # and it lives outside the background, so it must NOT enlarge its own box.
    gap = boxes["after_mb"][1] - boxes["mb"][3] - 1
    ck(gap >= 60 - 8,
       "margin-bottom:60px separates it from the next block (gap %d)" % gap)
    ck((boxes["mb"][3] - boxes["mb"][1] + 1) < 60,
       "margin-bottom stays OUTSIDE its own background (box height %d)"
       % (boxes["mb"][3] - boxes["mb"][1] + 1))

    # --- a nested background stays inside its padded parent ------------------
    il, it, ir, ib = boxes["wide_in"]
    ck(il > col_l and ir < col_r,
       "nested bg paints its OWN box, inset within the padded parent")
    ck(abs((il - col_l) - 40) <= 6,
       "nested bg is inset by the parent's 40px padding (got %d)" % (il - col_l))

    # --- the border is the OUTERMOST edge -----------------------------------
    bl, bt, br, bb_ = boxes["border"]
    pl, pt, pr, pb = boxes["bordered"]
    ck(bl <= pl and br >= pr,
       "border encloses its background horizontally (%d..%d vs %d..%d)" % (bl, br, pl, pr))
    ck(bt <= pt and bb_ >= pb,
       "border encloses its background vertically (%d..%d vs %d..%d)" % (bt, bb_, pt, pb))

    # --- vertical padding is INSIDE the background --------------------------
    # The padded block's background must be taller than one line box; a padding
    # band that leaked outside it (pre-M1900) left the background hugging the text.
    ck((pb - pt + 1) > 2 * 30,
       "padded block's background is taller than its 2x30px padding (got %d)" % (pb - pt + 1))

    print("%d checks, %d failures" % (checks, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
