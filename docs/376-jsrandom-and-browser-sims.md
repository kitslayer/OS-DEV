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

## M401–415: completing the utilities, and the parse-cap lesson

This stretch rounded out the shell's standard utilities — text (`fold`), number
theory (`gcd`/`primes`/`fib`/`fizzbuzz`/`stats`), base conversion (`base`/`dec`),
`roman`, `ascii`, `rot13`, and the data/time formatters (`size` bytes→GB/MB/KB,
`dur` seconds→d/h/m/s) — plus more browser pages (palette, a `Date`-driven
**clock** that confirmed the engine's `Date` is RTC-wired and live) and richer
calendars (`cal -y` full year, `cal -3` prev/current/next).

**Durable lesson (review #13 caught two real bugs):** the digit-parse idiom
matters. `while (d && x < CAP) x = x*10+d;` makes the cap the *loop condition*,
so the body runs once more after x crosses CAP and **x reaches ~10×CAP**. That
let `gcd` parse args to ~1e10 (cap 1e9) so `a*b` overflowed `long`, and `dec`
parse v to ~1.6e19 so `(long)v` printed negative. The safe idiom is to guard the
*assignment*: `while (d) { if (x < CAP) x = x*10+d; p++; }` — x freezes at ~10×CAP
and each command must size its downstream math (sum / division / iteration cap)
to absorb that overshoot. Fixes: tightened gcd's cap and printed dec's value
unsigned (`print_base(v,10)`). Review #14 then confirmed `stats`/`size`/`fib`
already use the safe idiom. Fourteen reviews this session, all SHIP after the
two #13 fixes — the periodic reviews keep earning their keep even on "trivial"
code.

## M416–420: the decode companions, and probing the engine to TWO real fixes

The shell's decoders rounded out their encoders: `unmorse` (Morse→text, an
exact-match table lookup), `unhex` (hex→ASCII), and `unbase64` (base64→text, a
6-bit accumulator over the A–Za–z0–9+/ alphabet). Review #15 SHIP'd all three —
the `unmorse` exact-match was *exhaustively* proven free of prefix false
positives (≈95 prefix collisions in the table, zero wrong matches) by emulating
every dot/dash string up to length 6.

Then the **gap-finding lesson paid off twice over.** Probing the JS engine from
the shell (`js -e print(...)` — note: `-e` does NOT auto-print, you need an
explicit `print`/`console.log`) across nine batches (~45 features) turned up two
genuine **correctness bugs** in the crown jewel, both fixed + reviewed:

1. **`[1] instanceof Array` → `false`** (M419). Arrays are `V_ARR`, not `V_OBJ`,
   and `Array` is a `V_NATIVE` ctor while `Object` is a `V_OBJ` — so the
   `instanceof` handler's "LHS must be V_OBJ, RHS must be V_FUN/V_NATIVE"
   early-return rejected both `[..] instanceof Array` and `{} instanceof Object`.
   Fix: record the two built-in ctor objects at setup (`g_array_ctor`/
   `g_object_ctor`) and, at the top of `case 'S'`, return true for an array vs
   `Array` and any array/object/function vs `Object`. Additive + fall-through:
   every other RHS (user classes, native Map/Set/Error/Date) has a distinct ctor
   object, so it reaches the existing `ctor_class`/`fn_proto` walk UNCHANGED.
   Primitives carry `.o==0` (all val ctors build from `UND()`), so the
   `b.o && b.o==g_*_ctor` guard can't spuriously match.

2. **`[1,2]+[3]` → `0`** (M420). The `+` handler only took the concat path for a
   literal `V_STR` operand, so two arrays hit `to_num(arr)+to_num(arr)=0`. Real
   JS does ToPrimitive first (objects stringify). The fix exploits the enum order
   — number-ish types (`V_UNDEF..V_NUM`) are `< V_STR`, and string + every
   object type are `>= V_STR` (Map/Set/Date keep `val.t==V_OBJ`) — so `==V_STR`
   becomes `>=V_STR` at both `+` sites. The numeric path is **byte-identical**
   for number-ish operands; only object operands (previously garbage `0`) are
   redirected to the already-proven concat path. `2+3`→5 and a numeric `reduce`
   stay numeric; `1+[2,3]`→"12,3" is now spec-correct.

**The bigger finding:** everything *else* probed is correct, including the subtle
cases people get wrong — `typeof null`→"object", default `.sort()` is
lexicographic (`[10,2,1]`→"1,10,2"), `0 ?? 5`→0 (nullish ≠ `||`), `'x'+[1,2,3]`
→"x1,2,3", optional chaining, spread-into-call, ES2021 `replaceAll`/`flatMap`,
ES2023 `findLast`. The engine is comprehensively modern AND correct; the only
known remaining gaps are getters/setters (hot path) and generators/async
(architecturally hard) — both deferred. Seventeen reviews this session, all SHIP.
**Probing > assuming, again: ~43 confirmations plus 2 real fixes.**
