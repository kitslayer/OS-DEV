# Milestone 134 — window minimize / restore (F3)

**Goal:** a desktop-environment improvement. The WM already had F2 (cycle focus),
F4 (maximize/restore), and F5/F6 (tile). Minimize was the obvious missing core
window operation — hide a window to just its taskbar chip and bring it back.

## How it works

- **`window_t` gains `minimized`** (added as the last field so the existing
  positional struct literals zero-fill it — new windows start un-minimized).
- **F3** (scancode `0x3D` → WM control code `0x18`, intercepted before app key
  routing like F2/F4): sets `minimized` on the focused (topmost) window and
  **sinks it to the bottom** of the z-order (new `sink_window`, the inverse of
  `raise_window`), so focus falls to the window below. Guarded by `win_count > 1`
  so the desktop always keeps a focusable window. Any in-progress drag/resize
  gesture index is cleared, since the z-order array just reordered (the same
  precaution the milestone-83 reap fix uses).
- **Rendering** skips minimized windows; the click hit-test skips them too (a
  hidden window isn't clickable/draggable).
- **Restore**: `raise_window` clears `minimized` whenever it lifts a window — so
  **F2** (cycle focus, which raises the bottom window) brings a minimized window
  back, and so does **clicking its taskbar chip**. A minimized window's chip is
  drawn dimmed so you can see it's hidden.

## Verified

Headless, keyboard-driven: at boot (Welcome, Files, Shell — Shell focused), **F3**
hides the Shell — it vanishes from the desktop, its taskbar chip dims, and focus
falls to Files (highlighted). **F2** restores the Shell — it reappears focused,
chip un-dimmed, showing its content. No panics.

## Review hardening (subagent review #26)

A review of this change (and the milestone-131 `tls_get` lock, confirmed correct)
found two issues, both fixed:

- **HIGH — focus could land on a minimized window.** The F3 guard was only
  `win_count > 1`, so minimizing repeatedly could hide *every* window, leaving the
  focused (topmost) one minimized and routing keys to a hidden window (recoverable
  via F2/chip, but wrong). Fixed: F3 now counts *visible* windows and only
  minimizes when more than one is visible, plus key routing swallows input when
  the top is (defensively) minimized.
- **MEDIUM — `raise_window` (F2, taskbar-chip click) reordered the z-order array
  without clearing an active drag/resize index** (pre-existing, adjacent): F2 or a
  chip click mid-drag would then move the wrong window. Fixed by clearing
  `dragging`/`resizing` at both call sites (matching what F3 and the reaper do).

## Files
- `kernel/keyboard.c` — F3 (`0x3D`) → control code `0x18`
- `kernel/desktop.c` — `window_t.minimized`; `sink_window`; F3 handler;
  `raise_window` clears `minimized`; render + click hit-test skip minimized;
  dimmed chip; Welcome hint updated
