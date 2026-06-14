# Milestone 152 — switch / do-while

Adds two control-flow statements to the JS interpreter (`kernel/js.c`):

- **`switch (x) { case A: …; default: … }`** — strict (`===`-style) matching on
  numbers/strings/booleans, **fall-through** (a case without `break` continues into
  the next), `default` anywhere (matched only if no case does), and `break`
  terminates. Statements run in a fresh scope; `return`/`continue` propagate out.
- **`do { … } while (cond)`** — runs the body at least once, then repeats while
  `cond` is truthy (same 5M-iteration safety cap as `while`/`for`).

Host-tested under ASan+UBSan: multi-case fall-through (`case 2: case 3:`),
mid-`switch` `default`, string `switch`, `break`, and do-while running once when
the condition is already false. New keywords: `switch case default do`.
