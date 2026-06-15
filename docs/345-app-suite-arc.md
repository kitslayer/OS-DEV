# 345–350: rounding out the app suite + discoverability

This arc added four apps in genres the suite lacked, plus the launcher/discovery
plumbing to reach them. The recurring theme: small isolated ring-3 programs, each
verified live, several reviewed by a subagent.

## Filling genre gaps

The 20 existing apps were action games, puzzles, a word game, an adventure, and
tools. Missing genres drove the picks:

- **Tic-Tac-Toe (M345)** — *adversarial AI*. The opponent is full-game-tree
  **minimax**, scored from O's view (`10 - depth` for an O win, `depth - 10` for
  an X win, 0 for a draw; O maximizes, X minimizes). The tree is tiny so no depth
  limit is needed, and scoring a faster win higher makes it finish quickly and
  stall a loss. It is provably **unbeatable** — the human can only draw or lose.
- **Blackjack (M346)** — *cards*. A Fisher-Yates-shuffled 52-card deck, aces that
  count 11 then demote to 1 while busting, a dealer that stands on (soft) 17, and
  a two-card natural paying 3:2. A review flagged that a player natural vs a
  dealer who *draws* to 21 should pay 3:2 (only a dealer *natural* pushes it) —
  fixed.
- **Typing test (M347)** — *skill*, not a game. WPM = `correct*12/elapsed`
  (= chars/5 over seconds/60), accuracy = correct/total, per-char green/red.
- **Simon (M349)** — *audio-visual memory*. A growing tone+colour sequence echoed
  on keys 1-4.

## The persistence pattern

The typing best-WPM, the bj bankroll, and the Simon best-run all reuse the games'
high-score idiom: a tiny `*.HI` file via `sys_readfile`/`sys_writefile`. Two
disciplines, both from review feedback: **clamp the parse** (`if (v > 100000)
break;`) so a corrupt/crafted file can't overflow `int`; and **gate the record**
where it could be gamed (the typing best only updates on a ≥90%-accuracy run).

## Discoverability

- **F9 menu (M348)** was a *hardcoded* list that stopped at Breakout — 13 apps
  were unreachable from it. Now it lists all of them. Its render, keyboard-nav,
  and hit-test were already dynamic over the count; one single column fits ~31
  entries above the taskbar, so it is now near capacity (a 2-column menu is the
  next step before many more apps).
- **`apps` (M350)** lists every program straight from the kernel's `progs[]`
  (single source of truth), so the shell has discovery too and the `run` error
  no longer hardcodes a stale subset.

## Lessons

- **Adding an app touches 5–6 spots**; the easy-to-miss one is `global
  <name>_elf_start` in `user_blob.asm` — without it the link fails with a cryptic
  "undefined reference" even though the ELF built.
- **The test harness appends Enter** to a typed command. Simon's "press any key
  to restart" caught that Enter and instantly dismissed the game-over *in
  testing* — a verification artifact, not a user-facing bug (a real keypress has
  no trailing Enter). The fix (require SPACE) is still the right call. Apps that
  filter input (printable-only, or specific commands) never noticed.
