# M264–271 — loose `==`, and probing a "finished" engine

## Loose `==` vs strict `===` (M271)

The engine had treated `==` and `===` as one operator (a shared parse-node op).
That made the most common JavaScript equality idioms wrong:

```js
x == null        // only matched null, never undefined
"5" == 5         // false (no coercion)
1 == true        // false
```

M271 splits them into four distinct operator codes — `===`/`!==` strict,
`==`/`!=` loose — and implements abstract equality properly:

- `===` / `!==` now route through `val_equal`, the same identity-aware predicate
  `Object.is` uses, so they are **truly strict**: same type required, objects by
  reference, `1 === true` → `false` (this also retired a stale bug where `===`
  used to coerce numbers/booleans).
- `==` / `!=` use a small recursive `loose_eq`: same type → strict; `null` and
  `undefined` are inter-equal but unequal to everything else (so `x == null` is
  the null-or-undefined check again); an object compared to a primitive coerces
  the object to a string and re-compares (bounded to one step); and
  number/string/boolean cross-type compare numerically.

```js
null == undefined   // true        null === undefined  // false
"5" == 5            // true        "5" === 5           // false
1 == true           // true        1 === true          // false
{} == {}            // false (identity, both == and ===)
[1] == 1            // true (object → "1" → 1)
```

The whole suite passed **unchanged** after the split — its existing `==`
usage was all same-type or null-compatible, so no golden lines moved.

## The meta-lesson: probe the basics, even late

By M263 the JS engine was, by every reasonable measure, "comprehensively
complete and multiply SHIP-reviewed" — full OOP, a prototype chain, the whole
operator set, a deep standard library. And yet a focused round of *probing the
basics* — running one-line edge cases through the host build and comparing to
the spec — turned up a string of **pre-existing, years-old correctness bugs in
fundamental operators**:

| Probe | Was | Should be | Fixed |
|-------|-----|-----------|-------|
| `{} === {}` | `true` | `false` | M266 — objects compared via `to_num`→0 |
| `"apple" < "banana"` | `false` | `true` | M267 — `<`/`>` coerced strings to `0` |
| `1 === true` | `true` | `false` | M271 — `===` had num/bool coercion |
| `x == null` (x undefined) | `false` | `true` | M271 — `==` wasn't loose |
| `arr.length = 2` | ignored | truncates | M267 |

None were caught by the existing suite, because the suite (like most test
suites) exercised the *common, correct-by-construction* paths — `a === a`,
same-type comparisons, sorting via `Array.sort` (which strcmps directly, not via
`<`). The bugs lived in the corners you only hit by *deliberately* testing the
distinct-object case, the two-different-strings case, the cross-type case.

Worth keeping in mind for any mature interpreter:

- **Test identity comparisons on two *distinct* objects**, not just `a === a`.
- **Test relational operators on non-numeric strings**, not just numbers.
- **Test the coercion matrix** (`null`/`undefined`/`""`/`0`/`true` cross-pairs).
- **Test the mutating setters** (`arr.length =`, …), not just the getters.
- Confirm the genuinely-dangerous ones are *safe*, not just correct: integer
  **div/mod by zero** here is guarded (`1/0`→`0`), not a SIGFPE — verify that
  rather than assume it.

A clean test suite is necessary but not sufficient; it mostly proves the paths
you *thought* to test. Adversarial probing of the primitives is a separate,
high-yield activity — and each fix here was small, contained, and gated by the
very suite that had missed the bug.

(Also in this arc: M264 `in`/`instanceof` walk the prototype chain; M265
`Object.defineProperties` + `Object.create(proto, descriptors)`; M266
`structuredClone`; M268 the `Array(n)`/`new Array(…)` constructor; M269/M270
`>>>` and `>>>=`, completing the bitwise and compound-assignment sets.)
