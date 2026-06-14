# Milestone 74 — 8th review: browser hardening

**Goal:** act on the 8th review subagent, run against the new browser code from
milestones 71–72 (chunked decode + keyboard link nav). It found one CRITICAL
memory-corruption bug and one correctness bug; both are fixed and verified.

## C1 (CRITICAL) — `dechunk` integer overflow → ~2 GB out-of-bounds `memmove`

`dechunk` parses an HTTP chunk size from the response body — **attacker-controlled
bytes**. The old code parsed the hex size into a signed `int sz` with no cap, then
guarded the copy with `if (in + sz > len) sz = len - in;`.

A hostile or buggy server can send a chunk-size line like `7ffffff0`. That makes
`sz` a large positive int, and `in + sz` **overflows to a negative value** — so
the `> len` guard is false, the truncation clamp is skipped, and `memmove` runs
with ~2 billion bytes. On this kernel (no memory protection) that shreds memory.

**Fix:** parse the size as `unsigned` with a cap (so `sz * 16` can't overflow),
and bound the copy against the *headroom* `len - in` instead of ever forming
`in + sz`:

```c
unsigned sz = 0;
... if (sz > (unsigned)RAW_MAX) sz = RAW_MAX; else sz = sz*16u + d;
...
unsigned room = (unsigned)(len - in);   /* in <= len here; never compute in+sz */
if (sz > room) sz = room;
```

Verified by re-running the host unit test with the hostile inputs added, under
`-fsanitize=address,undefined`: `7ffffff0\r\nABCD` (claims ~2 GB, 4 bytes present)
correctly yields `ABCD`; a 16-digit `ffffffff…` run is capped to `XY`; the RFC
example and truncation cases still pass — **0 failures, no sanitizer reports.**

## C2 (should-fix) — `select_link` read a stale scroll target

`b->linky[]` (each link's content-space y, used to scroll a selected link into
view) was written during render but **never cleared on a new page**. A link can
be counted in `nlink` yet never laid out — e.g. an empty anchor `<a href=x></a>`
bumps the link count but emits no word — so selecting it read a leftover y from a
previous page and jumped the scroll somewhere meaningless. Not a memory-safety
bug (the index is always in range), but wrong behavior.

**Fix:** `memset(b->linky, 0xFF, …)` (→ -1, "not laid out") in `parse_html`/
`parse_text`, and in `select_link` only scroll when `b->linky[s] >= 0`.

## Result

Clean build, clean boot (0 panics), and the keyboard link selection still
highlights and follows links correctly. Eight reviews now, every one has found at
least one real bug — the recurring theme this round being that **any size or
length parsed from the network must be treated as adversarial**: cap it, and
bound against remaining space, never against a sum that can overflow.

## Files
- `kernel/browser.c` — overflow-safe `dechunk`; `linky` cleared per page +
  guarded in `select_link`
