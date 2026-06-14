# Milestone 147 — arrow functions

**Goal:** add ES6 **arrow functions** to the JS interpreter — the idiomatic way to
write the callbacks that `map`/`filter`/`forEach` (milestone 146) take, and
ubiquitous in modern JavaScript.

## What

Both forms parse, in `kernel/js.c`'s `parse_assign`:
- single param: `x => x * x`
- parenthesized params: `(a, b) => a + b`, `() => 42`
- block body: `(n) => { var s = 0; for (...) s += i; return s; }`
- expression body becomes an implicit `return`.

They are ordinary closures at runtime (the engine has no `this`, so no rebinding
is needed) — reusing the existing `N_FUNC` value, so currying works:
`var add = a => b => a + b; add(10)(5) // 15`.

## How (the parsing subtlety)

`(a, b)` looks identical to a parenthesized expression until the `=>` after the
`)`. So `parse_assign` does **bounded lookahead**: it snapshots the lexer
(`lex_save`), tentatively parses an identifier list in parentheses, and only
commits to an arrow if `=>` follows — otherwise it `lex_restore`s and parses the
expression normally. This keeps plain parens (`(2+3)*4`, `((1+2))`) correct.

## Verified

Host-tested under ASan+UBSan: arrow forms, block bodies, IIFE `(x=>x+1)(10)`,
currying, **and** that plain parenthesized expressions still parse. Live in the OS:

```
js -e print([1,2,3,4,5].map(x => x*x).filter(x => x > 4).join(","))
9,16,25
```

## Files
- `kernel/js.c` — `lex_save`/`lex_restore`, `make_arrow`, arrow detection in
  `parse_assign`.
