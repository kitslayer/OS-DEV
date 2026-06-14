# Milestone 107 — clip-once `fb_fill_rect`

**Goal:** speed up the hottest drawing primitive. `fb_fill_rect` paints the
wallpaper, every window background, every title bar, the taskbar, buttons,
bevels, selection highlights — and it runs on every `render_scene` (i.e. every
window drag, every keystroke that redraws an app, every clock tick).

## The problem

It was a per-pixel loop over `fb_pixel`:

```c
for (j…) for (i…) fb_pixel(x+i, y+j, color);
```

and `fb_pixel` re-does **two checks for every single pixel**: a bounds test
(`(unsigned)x < fb_w && (unsigned)y < fb_h`) and a `target ? target : lfb`
branch to pick the draw destination. Filling the full-screen wallpaper is ~786 K
pixels, so that's ~786 K bounds tests + ~786 K destination branches **per scene
render** — pure overhead, since the rectangle and the destination are the same
for the whole fill.

## The fix

Clip the rectangle to the screen **once**, resolve the destination pointer
**once**, then fill with a tight loop:

```c
if (x < 0) { w += x; x = 0; }  …  if (y+h > fb_h) h = fb_h - y;
if (w <= 0 || h <= 0) return;
uint32_t *dst = target ? target : (uint32_t *)lfb;
for (j…) { uint32_t *row = dst + (y+j)*fb_w + x; for (i…) row[i] = color; }
```

The inner loop is now a plain store with no per-pixel branching, which the
compiler lowers to general-register stores (or `rep stos`) — `-mgeneral-regs-only`
keeps it off SSE, as elsewhere. Output is identical: the one-shot clip computes
exactly the same visible region the per-pixel bounds test did (including
left/top clipping for negative coordinates).

## Verified

Boots clean; the desktop renders **pixel-identically** — wallpaper gradient
(`vgrad` is a stack of `fb_fill_rect` rows), window title-bar gradients and
bevels, the taskbar, and the Apps button all correct — with no corruption and no
panics.

Together with milestones 105–106, the whole hot render/copy path is now
efficient: tight fills (107), `memcpy`/`memset` by the word (106), and a
cursor-only update that touches just two small rectangles (105).

## Files
- `kernel/fb.c` — `fb_fill_rect` clips once, resolves the target once, tight fill
