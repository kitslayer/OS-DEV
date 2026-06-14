# Milestone 169 — destructuring assignment

Completing the modern binding-syntax set begun with spread/rest (M168): `var`
declarations can now bind array and object **patterns**, with renaming,
defaults, rest, nesting, and `for-of` loop variables.

## What works

```js
// array
var [a, b, c] = [10, 20, 30];          // a=10, b=20, c=30
var [first, ...tail] = [1, 2, 3, 4];   // first=1, tail=[2,3,4]
var [x, y = 99] = [1];                  // x=1, y=99 (default for missing)

// object — shorthand, rename, default, rest
var { name, age } = { name: "OS", age: 2 };
var { name: nm } = { name: "DEV" };                 // rename -> nm
var { lang = "C" } = {};                            // default
var { a, ...others } = { a: 1, x: 2, y: 3 };        // others = {x:2, y:3}

// nested patterns
var [{ id }, [m, n]] = [{ id: 7 }, [8, 9]];         // id=7, m=8, n=9

// from a function result, and as a for-of loop variable
var [q, r] = pairReturningFn();
for (var [k, v] of [[1, 2], [3, 4]]) total += k * v;
```

## How it's implemented

The key insight: an array/object **pattern** has the same surface syntax as an
array/object **literal**, so the existing literal parsers produce the pattern
AST for free — including nesting, because they recurse.

- **Parsing.** In a `var` declarator, a leading `[` or `{` is parsed with
  `parse_primary` and stored as the declarator's pattern (`decl->b`) instead of a
  name. The object-literal parser gained a `{ x = default }` form (it builds an
  `N_ASSIGN` target). `for (var <pattern> of …)` is detected and stores the
  pattern on the loop node.
- **Binding.** A new recursive `bind_pattern(pattern, value, env)` walks the
  pattern and defines each leaf identifier:
  - `N_IDENT` → bind the value.
  - `N_ASSIGN` → a default: bind the right-hand side when the value is
    `undefined`, else the value (covers `[a=1]`, `{x=1}`, and `{k: x = 1}`).
  - `N_ARRAY` → bind element *i* to `value[i]`; an `N_SPREAD` element gathers the
    remaining elements into a fresh array.
  - `N_OBJECT` → bind each `value[key]` to its target; an `N_SPREAD` collects the
    own keys not already named into a fresh object.
  - Any sub-target may itself be a pattern, so nesting just recurses.
- Reused by both `var` declarations and `for-of` loop variables.

Destructuring a missing element / absent key yields `undefined`; destructuring a
non-array/non-object simply binds `undefined` (no throw). All allocations
(`new_obj`, `arr_push_val`, `obj_set`) are OOM-guarded and `bind_pattern` bails
on `g_oom`. Covered by the regression suite (`tests/js/suite.js`, `make jstest`,
ASan/UBSan-clean); the kernel builds clean.

## Not yet

Destructuring in plain assignment (`[a, b] = [b, a]` without `var`) and in
function parameters (`function f({ a, b }) {}`) are not wired up yet — both reuse
`bind_pattern`, so they're a small follow-up. Array holes (`[, x]`) are not
supported.
