# Milestone 178 — regular expressions (from scratch)

The JavaScript engine gained a from-scratch regular-expression engine and wired
it into `RegExp` plus the `String` methods that take a pattern. This is the
largest single addition since the interpreter itself, and the highest-value
remaining JS capability — a huge range of real scripts lean on regex.

## What works

```js
var re = new RegExp("\\d+");
re.test("abc123");                       // true
new RegExp("(\\d+)-(\\d+)").exec("12-345"); // ["12-345","12","345"]

"hello world".search(new RegExp("wor"));    // 6
"a1b2c3".match(new RegExp("\\d", "g"));      // ["1","2","3"]
"2024-01-15".replace(new RegExp("(\\d+)-(\\d+)-(\\d+)"), "$3/$2/$1"); // "15/01/2024"
"a b   c".replace(new RegExp("\\s+","g"), "_");   // "a_b_c"
"one, two ,three".split(new RegExp("\\s*,\\s*"));  // ["one","two","three"]
new RegExp("^[a-z]+$", "i").test("HelloWorld");    // true
```

**Supported syntax:** literal characters, `.` (any but newline), character
classes `[abc]` `[a-z]` `[^…]`, the shorthands `\d \w \s \D \W \S`, the greedy
quantifiers `* + ?`, alternation `|`, capture groups `(…)` and non-capturing
`(?:…)`, anchors `^ $`, and escapes (`\n \t \r \. \\` etc.). **Flags:** `i`
(case-insensitive) and `g` (global, with `lastIndex` advancing across
`test`/`exec` and driving `match`/`replace`/`split`). Replacement strings expand
`$&` (whole match), `$1`–`$9` (groups) and `$$`.

## How it's built — and why it can't hang the kernel

A pattern is parsed (recursive descent) into a small tree, then compiled to a
flat **instruction program** (`I_CHAR/I_ANY/I_CLASS/I_BOL/I_EOL/I_SAVE/I_SPLIT/
I_JMP/I_MATCH`). Matching is a **recursive backtracker** over that program with
two hard safety limits, which matters because both the pattern *and* the text
are attacker-controlled (page scripts):

- a **step budget** (300 000 instruction-steps per search start) — a pathological
  pattern like `(a+)+$` against a long non-matching string can't catastrophically
  backtrack; it exhausts the budget and cleanly reports no match (verified to
  finish in milliseconds, not hang);
- a **recursion-depth cap** (3 000) so the C stack can't overflow on the kernel's
  task stacks regardless of pattern shape;
- compilation is bounded to `RE_MAXPROG` instructions and `RE_MAXGROUP` capture
  groups; everything is arena-allocated (no leaks, reset per run).

The engine was developed and hard-tested **standalone first** (30 cases incl.
ReDoS, captures, classes, alternation — ASan/UBSan-clean) before being ported in,
then UBSan caught one integration bug (see below) which is fixed.

## Integration

A `RegExp` value is a `V_OBJ` whose `obj->kind == V_REGEX` and `obj->rx` holds the
compiled `regex`; `new RegExp(p, f)` and `RegExp(p, f)` both build one (native
constructors are supported by `new`). Method calls on a regex object dispatch to
`eval_regex_method` (`test`, `exec`); `.source`/`.global`/`.flags` are plain
properties. `String.prototype` `match`/`search`/`replace`/`split` detect a regex
argument (and `match`/`search` compile a string argument as a pattern, per JS).

## Known limitations (deferred)

- **Regex literals `/foo/g`** are not lexed yet — use `new RegExp("foo","g")`.
  Literal syntax needs context-sensitive lexing (regex vs. division) and is the
  next step.
- `match`/`exec` results don't expose the `.index` property: arrays here store
  elements in `vals[]` and can't also carry named properties without corrupting
  `.length` (this was the UBSan-caught bug — `obj_set` on an array scanned
  uninitialised `keys[]`; named-prop lookup on arrays was removed).
- No backreferences, lookahead/behind, `{n,m}` counted repetition, or named
  groups; `.` doesn't match newline (matching JS without the `s` flag).

Covered by the regression suite (`tests/js/suite.js`, `make jstest`,
ASan/UBSan-clean); the kernel builds clean.
