# Milestone 83 — 10th review: window-manager gesture fixes

**Goal:** act on the 10th review subagent, run against this session's WM changes
in `desktop.c` (the F2/F4 keyboard window management, the `window_t` struct
fields, and the keyboard-driven Files window). It confirmed those were sound and
found two real issues in the drag/resize gesture handling.

## #1 (should-fix) — stale `dragging`/`resizing` index after a mid-gesture reap

Each frame the WM reaps windows whose app has exited (`remove_window`, which
shifts the `windows[]` array down and decrements `win_count`). But the active
drag/resize gesture is tracked by an *index* (`dragging`/`resizing`). If a
window at a lower index exits while you're mid-drag — e.g. a snake game ends, or
the shell `exit`s — the array shifts but the index isn't adjusted, so the next
frame drags the *wrong* slot (a stale/dead window copy).

It's contained within the static `windows[16]` array (the index is only ever
`win_count-1` and removals shrink the range, so no out-of-bounds kernel write) —
hence should-fix, not critical. **Fix:** clear `dragging = resizing = -1`
whenever a `remove_window` happens during the reap loop, cancelling any gesture
whose target just moved.

## #2 (minor) — maximize lost after dragging a maximized window

`toggle_maximize` saves the pre-maximize geometry in `sx,sy,sw,sh`. If you then
dragged or resized a maximized window, `x/y/w/h` changed but `maximized` stayed
set and the saved geometry went stale, so a later F4 "restore" jumped to the old
saved box. **Fix:** clear `maximized` when a window is manually dragged or
resized (a manual move naturally ends the maximized state).

## Also: build cleanup

Adding the `maximized`/`sx..sh`/`fsel` fields made the positional `window_t`
struct literals trigger `-Wmissing-field-initializers` (benign — C zero-fills the
rest — but noisy). Added explicit `,0,0,0,0,0,0` to the four literals so the
build is warning-clean again.

## Confirmed safe by the review

Struct zero-init at every creation site; `files_key` after `spawn_browser` (the
static array never moves, and `top`/`w` aren't reused post-spawn); F2/F4 control
codes can't reach apps; the Files highlight fill is clipped by `fb_pixel`; the
`line[48]` and `url[32]` buffers are in bounds.

Ten reviews now, every one has found at least one real issue.

## Files
- `kernel/desktop.c` — reap clears the active gesture; drag/resize clears
  `maximized`; explicit field initializers
