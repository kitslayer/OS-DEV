# M376–387 — Math.random, browser games/sims, and the >64-file fix

This arc turned the from-scratch JS engine + browser from "comprehensively
interactive" into "runs real little programs," and shook out two genuine bugs
along the way. Notes for future work.

## Finding the one JS gap (M376)

The engine's stdlib was assumed "comprehensive," but assumptions are cheap —
so I *probed* it from the shell with `js -e`: across three batches I tested 20
common features (map/filter/reduce, split/replace/repeat/slice/trim, sort with
a comparator, Object.keys, try/catch/throw, Date, JSON.stringify, indexOf,
toString(radix), padStart, flat, findIndex, charCodeAt, String.fromCharCode).
**All worked except `Math.random()`** — that was the single gap.

The fix had a wrinkle: the engine is **integer-only** (no FPU — `Math.floor`
etc. are identity). A standard `Math.random()` → [0,1) float is meaningless
here. So the contract became `Math.random(n)` → uniform integer in `[0,n)` (a
die/range) and the no-arg form → `[0, 2^31)` (for `% n`). Backed by xorshift64
seeded lazily from `rdtsc` (so sequences differ per boot — verified two boots
gave different rolls). The mod-by-zero/negative trap is avoided by testing
`m > 0` *before* the unsigned cast.

**Lesson:** probe before assuming; and a feature's *contract* must fit the
engine's model (integer RNG, not float).

## Browser games/sims are just `<script>` fn + onclick + persistent env

With Math.random in hand, five baked pages (file:rps/base/guess/ascii/life.htm)
showed the pattern is fully general:
- a load-time `<script>` defines functions + state (`var secret`, `var g`),
- inline `onclick="fn()"` calls them, and **state persists across clicks**
  (the per-page JS env from M287) — no localStorage needed,
- the DOM is updated via `getElementById().textContent` / `document.write`.

Rock-Paper-Scissors (persistent score), a number base converter
(parseInt+toString(radix)), Guess-the-Number (persistent secret), an ASCII
table (a load-time `document.write` loop), and **Conway's Game of Life** (2-D
arrays + 8-neighbour counting + Math.random seed, rendered as a `<pre>`, Step
advances a generation) all work. Life is the high-water mark: a real cellular
automaton running on the from-scratch engine.

**Test-harness lesson (now in the [[drive-harness]] memory):** `key:n` sends a
raw keypress (highlight, no Enter) vs `type:n` (n+Enter = highlight+activate).
Button-only pages drive with `type:n`; reaching a button *past* an input needs
two `key:n` after finishing typing (the highlight resets on finish).

## The >64-file bug (M385) and a corruption red herring

Adding the 5th page pushed the baked-file count past 64 and surfaced a real
bug: mkfatfs's `ent[64]` array was filled by an **unguarded** inline-files loop
(the host-files loop had an `ne<64` guard; the first loop didn't), so the 65th
file overflowed it (UB; the file came out "not found"). The FAT32 root dir is
already a multi-cluster chain sized to the file count, and the kernel follows
it — so the only fix needed was the array. First `ent[128]`; then, on review,
sized **exactly** `ent[NUM_FILES + NUM_HOST]` so it auto-grows and can never
overflow again (better than a magic constant + a silent-drop guard).

A scare followed: a later boot showed an empty Files panel and missing files —
but a fresh `make` had them. This is the **deferred FAT32 write-corruption**
(M367) accumulating over a session's many test-boots in the *local*
`build/fat.img`. `/build/` is gitignored, so the committed source (which
regenerates a clean 75-file disk) and the user's fresh build are unaffected —
it's a local-test artifact, not a source bug. **Rebuild before disk-dependent
tests.**

## Reviews

Ten subagent reviews across the session, all SHIP. The ones here (c4+uniq,
grep-v/sort-r/cut, Math.random/crc32/cal, tr/wc/mkfatfs) found no
memory-safety bugs; the durable output was the mkfatfs exact-sizing and a
couple of cut edge-case fixes (empty-slice trailing line, overflow caps).

## Continued: M388–400 (the demo suite + CLI tools + date tools)

The browser-program pattern was pushed to a **thirteen-page demo suite** —
games (RPS, Guess, Slot, 8-Ball), a simulation (Life + a Glider seed), a stats
visualisation (the 2d6 bell-curve histogram), and tools (base converter, ROT13,
UUID, number-facts, day-of-week, password generator). Each is a tiny baked page
exercising the engine + DOM; together they're the visible payoff of M376's
Math.random + the persistent per-page env.

Two genuine engineering points from this stretch:

1. **Gap-finding > assuming.** I kept probing the JS stdlib with `js -e` (28
   common features across several batches: map/filter/reduce, JSON.parse/
   stringify, Object.keys/values/entries, Array some/every/fill/reverse/
   indexOf/findIndex, String split/replace/slice/trim/padStart/repeat/
   toString(radix)/charCodeAt/fromCharCode, flat, try/catch, Date). **All work
   — Math.random was the only gap.** The engine is genuinely comprehensive;
   this is now a settled fact, not an assumption.

2. **CLI ↔ browser companions.** Several tools got both surfaces: `genpass` ↔
   passgen.htm, `uuidgen` ↔ uuid.htm, `weekday` ↔ weekday.htm. Plus pure-CLI
   `dur` (seconds → d/h/m/s) and `cal -y YEAR` (full-year calendar), rounding
   out the shell's date/time utilities beside `cal`/`cal MM YYYY`.

Verification lesson reinforced: 1-button / 1-input pages drive cleanly via the
harness; multi-button apps (a full calculator, a card-match grid) are NOT
reliably harness-verifiable, so they were deliberately skipped in favour of
verifiable designs. The remaining high-value work (enforcing cert validation,
inline remote images, shell pipes, the app-exit vmm teardown, FAT32 write
robustness) is genuinely risky and stays deferred to protect the working
browser/kernel/disk — a focused session each, not end-of-session attempts.
