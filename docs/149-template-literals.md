# Milestone 149 — template literals

**Goal:** add ES6 **template literals** — `` `Hello ${name}, ${1+2} items` `` — the
modern, readable way to build strings, and pervasive in real JavaScript.

## What

Backtick strings with `${ expr }` substitutions, in `kernel/js.c`:
- Any expression inside `${ }` (arithmetic, calls, `arr.map(...).join(...)`, …).
- **Nested** template literals: `` `a ${`b ${1+1}`}` `` → `a b 2`.
- Escapes: `` \` ``, `\$`, `\n`, `\t`, `\r`, `\\`.

## How

- **Lexer**: on `` ` ``, scan to the closing backtick while balancing `${ }` (so a
  `}` inside a substitution — e.g. an object literal — doesn't end it) and honoring
  `\` escapes. Emits a `T_TEMPLATE` token holding the raw inner text.
- **Parser** (`parse_template`): splits the raw text into literal runs and `${expr}`
  substitutions, building a left-associative `+` chain. Each `${expr}` is parsed by
  a **sub-lexer** over that slice (`parse_assign`). The chain always begins with a
  (possibly empty) string literal, so it coerces to a string — `` `${42}` `` is the
  string `"42"`, not the number.

## Verified

Host-tested under ASan+UBSan, live in the OS:

```
var n = "OS-DEV";
print(`hi ${n}, ${[1,2,3].map(x => x*x).join(",")}`)   // hi OS-DEV, 1,4,9
```
plus nested templates, `${expr}` with arrows/methods, and `` \` `` / `\${` escapes.

## Files
- `kernel/js.c` — `T_TEMPLATE` lexing, `parse_template`, `mkbin_plus`.
