# Milestone 40 — Concurrency hardening (browser worker)

**Goal:** fix the real concurrency bugs in the async browser (M36) that a code
review turned up. No new feature — this milestone is about *correctness* of code
that two threads (the window manager and the fetch worker) touch at once.

## The setup

OS-DEV's scheduler is **preemptive**: the timer IRQ can switch tasks between any
two instructions. So the window-manager thread and the fetch-worker thread can
interleave at the worst possible moments. The original M36 code coordinated them
with `volatile` flags only — which guarantees *visibility* on a single CPU but
**not atomicity** of multi-step operations. That left two genuine bugs.

## Bug 1 — double-free / leak when closing a tab mid-load (Critical)

Closing a browser window while its page was still downloading ran this race:

- `browser_destroy` (WM thread): "is it still `loading`? then don't free, let the
  worker free it."
- `worker_fetch` (worker thread): "is it `closed`? then I free it; otherwise I
  publish the result."

Each side read one flag and wrote another, with no interlock. Interleave them and
you get either **nobody frees** (a multi-KB leak) or **both free** (double-free →
heap corruption). The fix makes the decision atomic: both the close path and the
worker's finish path run their flag-read + flag-write inside a short
**interrupts-off critical section** (`irq_save`/`irq_restore`), so exactly one of
them is ever designated the owner of the free. A 24-iteration create-then-close-
mid-fetch stress test now reports **created == freed** with no faults.

## Bug 2 — lost / dropped loads (Should-fix)

The "one fetch at a time" guard (`if (g_busy || g_req) bail`) checked and then
published `g_req` non-atomically, and a second browser's load could be refused
forever (it showed "busy" and never retried). Fixes:

- **`claim_fetch()`** does the check-and-claim atomically (interrupts off).
- A refused load now sets a **`want` flag**; the WM retries it from
  `browser_poll()` once the worker frees up — so a load is never silently lost.
- **`browser_back()`** now *claims first, then* pops the history entry — before,
  a back that lost the race consumed a history entry without navigating.
- **`browser_destroy()`** cancels a not-yet-started request (`g_req == b`) so a
  closed browser's pointer can't linger in the queue.

## Takeaway

`volatile` is necessary but not sufficient: it stops the compiler caching a flag
in a register, but it does nothing for "read A, then write B" sequences that must
be atomic against a preempting thread. Those need real mutual exclusion. On a
single-CPU kernel, briefly disabling interrupts is the cheap, correct tool — and
exactly what the scheduler itself uses to protect the ready ring.

(Also corrected a stale comment in `task.c` that still described the scheduler as
"cooperative"; it has been preemptive since milestone 11, and the safety of this
code depends on knowing that.)

## Files
- `kernel/browser.c` — `irq_save`/`irq_restore`, `claim_fetch`, atomic worker
  pickup + finish, single-owner `browser_destroy`, the `want` retry, `browser_back`
- `kernel/task.c` — corrected scheduler comment
