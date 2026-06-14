# Milestone 79 — 9th review: Back URL race fix

**Goal:** act on the 9th review subagent, run against the M77–M78 browser
changes (bookmark-add, the `browser_back` local/network split, `<pre>`, and the
render clip). It cleared most of the new code and found one real **should-fix**
concurrency bug — a *pre-existing* one living in the function I'd just changed.

## The bug — `browser_back` network path: claim-before-set TOCTOU

The fetch worker is a separate task; it polls `g_req` every ~10 ms (PIT 100 Hz,
preemptive). The network branch of `browser_back` did:

```c
if (!claim_fetch(b)) return;            /* publishes g_req = b */
copy_url(b->url, b->hist[--b->histn]);  /* ...THEN sets the target URL */
```

`claim_fetch` publishes `g_req = b` **before** `b->url` is updated to the
back-destination. If a timer IRQ preempts the WM task in that few-instruction
window, the worker picks up `g_req` and reads the *stale* `b->url` — which still
equals the current page. Result: **Back re-fetches the page you're already on**
instead of the previous one. Every other navigation (`browser_navigate`,
`goto_href`) sets `b->url` *first*, then claims — the back path was the lone
exception, which is exactly why it was wrong.

## The fix

Mirror the proven set-then-claim ordering:

```c
copy_url(b->url, dest);                          /* set target BEFORE claiming */
if (!claim_fetch(b)) { copy_url(b->url, b->cur); return; }  /* lost race: restore */
b->histn--;                                      /* pop only after a successful claim */
```

On a lost claim it restores `b->url` to the current page and leaves history
untouched.

## What the review confirmed safe

The other four pieces were checked carefully and found sound: the new
**local-path Back** (its `irq_save` gate means no worker can be running for any
tab when it renders locally; the `dest` pointer stays valid across `histn--`);
**`browser_bookmark`** (buffer bounds, dedup-loop termination, `n==0` case, and
no VFS/`b->cur` reentrancy since both run on the WM task); the **`inpre`**
parsing (token/text bounds guarded, no `wstart` desync across the pre boundary,
`uint16_t` fields can't overflow given `TEXT_MAX < 65536`); and the **render
clip** (`dl` always in `[0, len]`, the negative-width degenerate case clamped).

## Verified (headless, by screenshot)

home → (select) **info.cern.ch** → follow its first link → **The WWW Project**
(`…/TheProject.html`) → **Backspace** → returns to **`http://info.cern.ch`** (the
correct previous page, re-fetched at 878 b — not the stale TheProject page).
Also re-confirmed: local Back to "home" still renders instantly, and `run calc`
still spawns/focuses after the WM changes.

Nine reviews now, every one has found at least one real bug.

## Files
- `kernel/browser.c` — `browser_back` network path: set `b->url` before
  `claim_fetch`, restore on lost claim
