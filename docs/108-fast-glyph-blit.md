# Milestone 108 — fast glyph blit (the last hot draw primitive)

**Goal:** finish the render-primitive optimization sweep. Text is everywhere —
the shell and its scrollback, the in-window terminal, browser pages and the
address bar, app grids, every window title, the file list, the taskbar. Each
8×16 glyph was 128 calls to the bounds-checked, target-branching `fb_pixel`.

## The two text paths

- **`fb_glyph(x, y, c, fg, bg)`** — *opaque* text (writes every pixel, fg or bg).
  Used for the terminal / app / browser character grids. A full text window is
  hundreds of glyphs × 128 `fb_pixel` calls per `render_scene`.
- **`draw_text` in desktop.c** — *transparent* labels (only the set pixels, over
  the existing scene). Used for titles, the file list, the taskbar, etc. It had
  its own per-pixel `fb_pixel` loop.

## The fix

Both now take an **in-bounds fast path**: when the whole glyph fits on screen
(the overwhelmingly common case — windows are on-screen), resolve the
destination pointer once and write directly with no per-pixel bounds test or
`target?:lfb` branch; otherwise fall back to the original clipped `fb_pixel`
loop for the rare straddling-the-edge glyph.

- `fb_glyph` gained the fast path in place.
- A new `fb_glyph_fg(x, y, c, fg)` in fb.c does the transparent version (set
  pixels only), and desktop.c's `draw_text` is now just a loop over it — the
  glyph bit-twiddling lives in fb.c where the framebuffer pointer is in scope.

The inner loop is plain register stores (`-mgeneral-regs-only` keeps it off SSE,
as with the other primitives).

## Verified

Boots clean; text renders **identically** — the green opaque Shell grid
(`fb_glyph`) and every transparent UI label (window titles, the `README.TXT…`
file list, the `Apps`/`Welcome`/`Files`/`Shell` taskbar chips, the clock, the
Welcome body) via `draw_text`→`fb_glyph_fg`. No corruption, no panics.

This completes the hot-path sweep across milestones 105–108: cursor-only updates
touch two small rects (105), copies go by the word (106), fills clip once and
run tight (107), and glyphs blit straight to the buffer (108).

## Files
- `kernel/fb.c`, `kernel/include/fb.h` — `fb_glyph` fast path; new `fb_glyph_fg`
- `kernel/desktop.c` — `draw_text` now calls `fb_glyph_fg`
