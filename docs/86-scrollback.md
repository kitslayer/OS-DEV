# Milestone 86 — terminal scrollback

**Goal:** stop losing output. The windowed apps render a fixed 17-row text grid;
when it scrolled, the top line was discarded forever. Now scrolled-off lines are
kept and you can **PgUp/PgDn** through the history.

![the shell scrolled up — earlier `ls` output recovered, with a `^` indicator top-right](osdev-scrollback.png)

## How it works

Each app keeps a `sb[48][cols]` scrollback ring plus a `view` offset (rows
scrolled up from the live bottom, 0 = live):

- `grid_scroll` — when the top line is about to scroll off, it's copied into the
  scrollback ring first (the ring shifts when full). If the user is currently
  scrolled up, `view` advances in step so the displayed lines stay put as output
  streams underneath.
- `app_render` — draws a 17-row window into the combined `[scrollback … live
  grid]` sequence at the current `view`, and shows a `^` marker in the top-right
  when scrolled up.
- `app_key` — **PgUp/PgDn** (new scancodes decoded in `keyboard.c` → control
  codes `0x15`/`0x16`) adjust `view` by 4 rows and are consumed as UI controls —
  *the program never sees them*. Any other key snaps back to the live bottom, so
  typing always returns to the prompt.

Crucially, the delicate line-reader/`grid_erase` logic is **untouched** — it
still operates on the live grid. Scrollback is purely a capture-on-scroll plus a
view offset, so it can't reintroduce input-editing bugs. PgUp/PgDn also page the
**browser** (alongside the existing space/b), for consistency.

## Verified (headless, by screenshot)

In the shell: ran `ls` three times + `help` to push well past 17 rows. The live
view shows the latest (`help`) output at the bottom, no indicator. **PgUp ×3**
scrolls up to reveal earlier `ls` listings that had scrolled off, with the `^`
marker in the corner. Typing returns to the bottom.

## Files
- `kernel/app.c` — `sb`/`view` fields, scrollback capture in `grid_scroll`,
  windowed `app_render`, PgUp/PgDn handling + snap-to-bottom in `app_key`
- `kernel/keyboard.c` — decode PgUp (`0x15`) / PgDn (`0x16`)
- `kernel/browser.c` — PgUp/PgDn page scrolling
