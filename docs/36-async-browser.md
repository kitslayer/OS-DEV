# Milestone 36 — Non-blocking page loads

**Goal:** stop the desktop from freezing while a page loads. An HTTP fetch can
take up to a second; until now the browser did it **on the window-manager's own
thread**, so the whole UI — cursor included — locked up until the page arrived.
Now the fetch runs on a separate **worker task** and the desktop stays live.

## The shape of the fix

This is a classic producer/consumer split across two threads:

- **A worker task** (one, created on first browser open, pinned to the kernel
  address space) loops waiting for a fetch request. When one appears it calls
  the blocking `http_get` into the browser's `raw` buffer, then flags the result
  as ready. It only ever *fetches* — it never touches the parsed/rendered state.
- **The window manager** keeps doing its normal thing: compositing every frame,
  moving the cursor, handling clicks. Each frame it calls `browser_poll()`,
  which — if the worker has delivered bytes — parses the HTML and triggers a
  repaint. Because the **parse happens on the WM thread**, parsing and rendering
  never run at the same time, so there's no data race on the token list.

While a page is in flight the browser shows **"Loading…"**; the cursor and the
rest of the desktop stay fully responsive because the preemptive scheduler
time-slices between the WM and the worker.

```
click a link / press Enter
   └─ browser_navigate(): set loading=1, hand the request to the worker, return
        (WM keeps compositing "Loading…", cursor live)
   worker task: http_get(...) ──fills raw──> set need_parse=1, loading=0
   WM next frame: browser_poll() ──parses raw──> renders the page
```

## The tricky part: closing a tab mid-load

If you close the browser window while its fetch is still running, freeing its
buffers would leave the worker writing into freed memory (use-after-free). The
fix is an ownership handshake:

- closing while `loading` is set doesn't free — it marks the browser `closed`
  and returns;
- the worker, when it finishes, checks `closed` **before** clearing `loading`:
  if the window was closed it frees the buffers itself; otherwise it publishes
  the result.

Because `loading` is only ever cleared by the worker (and only in the
not-closed path), the WM can never free a browser the worker is still using.
A small thing, but it's exactly the kind of lifetime bug that makes concurrency
hard — worth getting right.

## Limits kept honest
- Still **one fetch at a time** (a second navigation while one is in flight
  shows "busy, retry"). A real browser fetches many resources at once; a
  connection table is future work.
- The fetch's inner receive loop still polls the NIC busily, but preemption
  means the WM keeps running anyway — the desktop no longer freezes.

## Files
- `kernel/browser.c` — the worker task, `browser_navigate` (now async),
  `browser_poll` (WM-side parse), and the close-during-load handshake
- `kernel/desktop.c` — calls `browser_poll` for each browser window every frame
