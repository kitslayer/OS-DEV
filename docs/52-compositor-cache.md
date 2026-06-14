# Milestone 52 — Compositor optimization (scene cache)

**Goal:** make the desktop cheap to repaint. The compositor was re-rendering the
*entire* scene on every frame that anything changed — including every mouse
move. That's a lot of wasted work.

## The problem

`composite()` rebuilt the whole screen each time: a full-screen **wallpaper
gradient** (a `lerp` + a row-fill for all 768 rows), then every window with two
**soft-shadow** passes (`darken` reads *and* writes each shadow pixel), then the
cursor, then the blit to the framebuffer. That's on the order of **two million
pixel operations per frame** — and the window-manager loop marked the scene
dirty on *every cursor move*, so just sliding the mouse re-rendered the entire
desktop continuously.

## The fix: cache the rendered scene

Split the work in two:

- **`render_scene()`** draws the wallpaper + windows + taskbar into a **cached
  scene buffer** (no cursor). This is the expensive part — and it now runs *only
  when the scene actually changes* (a window moved, the clock ticked, a menu
  opened, a page loaded).
- **`present_frame()`** copies the cached scene to the back buffer, draws the
  cursor on top, and blits. It's just two `memcpy`s plus a tiny cursor — no
  gradients, no shadow math.

The window-manager loop now distinguishes *the scene changed* (`dirty` →
`render_scene`) from *only the cursor moved* (`moved` → just `present_frame`).
So moving the mouse around the desktop went from "recompute ~2M pixels" to "copy
a buffer" — roughly an order of magnitude less work per mouse frame — while the
heavy render happens only a few times a second.

A nice side effect: because `present_frame` repaints the back buffer from the
clean cached scene every time, the old cursor position is erased for free — no
"save-under" bookkeeping and no cursor trails are even possible.

(Dragging or resizing a window *does* change the scene, so those still trigger a
re-render while the mouse moves — handled explicitly.)

## Cost

One extra screen-sized buffer (the scene cache, ~3 MB alongside the existing
back buffer). A further optimization — blitting only the cursor's rectangle
instead of the whole buffer — is noted for later; the scene cache is the big win.

## Files
- `kernel/desktop.c` — `render_scene` / `present_frame` split, the scene buffer,
  and the `dirty` vs `moved` loop logic
