# Milestone 179 — regex hardening (CRITICAL review fixes)

A dedicated security review of the M178 regex engine found **two CRITICAL,
remotely-triggerable kernel-level stack overflows**. Both are fixed here. They
shared one root cause: recursion-depth limits sized for the review/host's 8 MB
stack, not the kernel's **256 KB JS task stack — which has no guard page**, so an
overflow silently corrupts adjacent kernel memory instead of faulting cleanly. A
web page's `<script>` could trigger either with a ~2–3 KB pattern.

## CRITICAL-1 — unbounded regex *parser* recursion

`rx_alt`/`rx_cat`/`rx_rep`/`rx_atom` are mutually recursive (a `(` group sends
`rx_atom` back into `rx_alt`), and the regex *pattern* is an ordinary JS string,
so the interpreter's `MAXDEPTH=120` guard never applied to it — the parser had no
depth guard of its own. `RE_MAXPROG=512` didn't help because the whole parse
*tree* is built before any instruction is emitted. ~1700 nested groups
(`"(" ×1700`) overflowed the 256 KB stack.

**Fix:** a depth counter in the `rparse` state, incremented/decremented in
`rx_alt` (the per-group recursion point), capped at **400** — far below the
~1700 cliff. Past the cap the parse fails cleanly (`P->err`), so the regex just
doesn't compile (`test`/`match` return false/null).

## CRITICAL-2 — matcher depth cap too high for the kernel stack

`re_run` recurses on every `I_SPLIT`/`I_SAVE`, so a greedy quantifier matching N
characters recurses ~N deep. Its cap was `depth>3000`, but the 256 KB stack
overflows around depth ~2000 — before the cap fired. (The 300 k step *budget*
doesn't help: a linear match spends only ~N steps while recursing N deep.)

**Fix:** lower the cap to **900**, comfortably under the ~2000 cliff with the
interpreter's eval frames already on the stack. A single run longer than ~900
characters now fails to match (returns no-match) rather than corrupting memory.
In practice this is rarely hit — real patterns match short tokens, and even on
long inputs each individual match is short (e.g. `\s+` matches a few spaces);
only a pattern trying to match one >900-char run is affected.

## Verification

The patterns the reviewer proved would overflow were run **in the real kernel**
(`RXTEST.JS`, `js rxtest.js`):

```
deep groups: false     (1800 groups — parser capped, compiled-failed)
long match: true       (a+ over 2500 chars — matcher capped, no crash)
redos: false           ((a+)+$ — step-budget, no hang)
normal: 42:id          (ordinary regex with capture groups still works)
SURVIVED               (OS did not crash; shell responsive afterward)
```

Host-side: the parser guard fires at >400 groups (300 still compiles), the
matcher caps without crashing, normal regex is unaffected, `make jstest` passes
ASan/UBSan-clean.

The review confirmed all other categories CLEAN (malformed patterns,
`RE_MAXPROG`/capture bounds, the step budget vs. catastrophic backtracking,
zero-width-match termination, the `sbuild` buffer, OOM, the class bitmap,
`lastIndex`). Remaining items were LOW non-safety notes (e.g. `split(/""/g)`
doesn't split to chars; `.index` omitted) — left as documented deviations.

**Lesson (recorded in memory):** depth/stack limits in any recursive engine must
be sized for the kernel's 256 KB guard-page-less task stack, not a host's 8 MB —
and untrusted-input recursion that bypasses the main interpreter's `MAXDEPTH`
(here, the pattern string) needs its own guard.
