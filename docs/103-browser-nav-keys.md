# Milestone 103 — browser navigation keys (home, bottom)

**Goal:** two small but genuinely handy keyboard shortcuts that round out the
browser's navigation.

![pressing `h` from a page jumps to the start page](osdev-gohome.png)

## What was added

In `browser_key` (non-editing mode):

- **`h`** — go to the **start page** (`home`), as a normal navigation (so
  Backspace returns to where you were). A quick "home" that previously needed
  editing the address bar by hand.
- **`G`** — scroll to the **bottom** of the page (the complement of `g`, which
  jumps to the top). It sets the scroll past the end; the render's existing
  clamp pins it to the real maximum.

## Verified (headless, by screenshot)

From `file:list.htm`, pressing **`h`** loads the **OS-DEV start page** (address
bar shows `home`) — confirming the home shortcut and that it's a real navigation.
No panics.

The browser's keyboard map is now: space/`f`/PgDn page down, `b`/PgUp page up,
`j`/`k`/arrows line scroll, `g`/`G` top/bottom, `h` home, `r` reload, `s` save,
`u` view-source, `a` bookmark, `\` find, `/`·`e` edit address, Tab/`n`/`p` link
select, Enter follow, Backspace back.

## Files
- `kernel/browser.c` — the `h` and `G` cases
