# Milestone 157 — persistent page state (localStorage)

**Goal:** let in-page JavaScript keep state across clicks — so an interactive page
can have a counter that actually counts, not just self-contained one-shots.

## The problem

Each `javascript:` click (and each page-load script) runs in a **fresh** `js_run`
— the interpreter's 1 MB arena and globals are reset every time (this is what keeps
memory bounded and the OOM/recursion hardening simple). So variables don't survive
between clicks. A stateful widget needs a store that *outlives* the arena reset.

## What

A from-scratch **`localStorage`** (the real web API shape: `getItem(key)` /
`setItem(key, value)`), backed by the **browser**, not the JS arena:

- `kernel/js.c` — a `localStorage` object whose `getItem`/`setItem` call host
  function pointers (`g_ls_get`/`g_ls_set`); `getItem` returns the stored string
  (interned out of the host buffer) or `null`. The shell `js` path leaves the hooks
  null (no store); the browser binds them via `js_set_storage`.
- `kernel/browser.c` — a per-page key→value store (`ls_keys`/`ls_vals`, up to 16
  entries) that survives the per-run resets and is cleared on navigation. Bound
  before every page-script / click-handler run.

## Verified (live in the OS)

`JSCLICK.HTM` has a counter link:
```js
var c = (parseInt(localStorage.getItem('c')) || 0) + 1;
localStorage.setItem('c', c);
document.write('<p>counter = ' + c + '</p>');
```
Clicking it repeatedly prints `counter = 1`, `counter = 2`, `counter = 3`, … — the
value persists across the independent click runs. Host-tested under ASan+UBSan
(round-trip set/get, `null` for missing keys).

## Files
- `kernel/js.c` (`localStorage`, `js_set_storage`), `kernel/include/js.h`,
  `kernel/browser.c` (the backing store), `tools/mkfatfs.c` (counter demo).
