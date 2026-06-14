# Milestone 180 — regex literals (`/pattern/flags`)

Regular expressions can now be written with the idiomatic literal syntax instead
of `new RegExp("…")`, which is how the overwhelming majority of real code uses
them:

```js
/\d+/.test("abc123");                  // true
"a1b2c3".match(/\d/g);                  // ["1","2","3"]
"2024-01-15".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1");
"a, b ,c".split(/\s*,\s*/);
/^[a-z]+$/i.test("HELLO");
var re = /(\w+)@(\w+)/;
```

## The lexer challenge: `/` is ambiguous

`/` is either the start of a regex literal or a division operator, and only
context disambiguates them — `a / b` is division, `= /b/` is a regex. JavaScript
lexers resolve this from the **previous token**: a `/` is division when the
preceding token *ends an operand*, and a regex literal otherwise.

The tokenizer now tracks the last token it produced (`lexer.last`, saved/restored
across the speculative arrow-detection lookahead). When it sees a `/`, it treats
it as **division** iff the previous token is a number, string, template literal,
identifier, regex literal, `)`, `]`, or postfix `++`/`--`, or a value keyword
(`this`/`true`/`false`/`null`/`undefined`/`super`); **otherwise** it scans a regex
literal — reading to the closing `/` while respecting `\`-escapes and `[…]`
classes (where `/` is literal), then the trailing `i`/`g` flags. (`//` and `/*`
comments are still consumed earlier, and `/=` still lexes as divide-assign in
division context.)

A `T_REGEX` token carries the pattern and a flag bitmask; `parse_primary` turns it
into an `N_REGEX` node, which at eval builds a fresh `RegExp` object via the same
`make_regex_val` used by `new RegExp(…)` (a literal makes a new object each time
it's evaluated, matching JS).

## Verification

Division was regression-tested heavily and is fully preserved:
`10/2`, `a/b`, `(6+4)/2`, `arr[0]/arr[1]`, `f()/3`, `o.v/2`, `"x".length/1`,
`i++/2`, `5/2/1`, `x/=5`, and `/x/g.test(s), 6/3` on one line all give the right
answers. Regex literals were tested for the tricky cases: escaped slash `/a\/b/`,
a slash inside a class `/[/]/`, a regex right after `if(`, and a regex in an arrow
body `s => /\s/.test(s)`. The `REGEX.JS` disk demo was rewritten to use literal
syntax and **verified in the real kernel**. `make jstest` passes ASan/UBSan-clean;
the kernel builds clean.

## Notes

The `}`-as-operand-end case is deliberately treated as regex context (so
`function f(){} /re/` works), at the cost of the rare `({})/x`. Multi-line regex
literals aren't supported (a newline ends the scan). With literals, the from-scratch
regex feature (engine M178, hardening M179) is now ergonomically complete.
