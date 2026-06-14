# Milestone 172 — assignment-target destructuring

The last piece of the destructuring story: destructuring into **already-declared
targets** (no `var`), which is what enables the no-temp-variable swap and other
in-place rebindings.

## What works

```js
// the classic swap — no temporary
var a = 1, b = 2;
[a, b] = [b, a];          // a=2, b=1

// three-way rotate
var p = 1, q = 2, r = 3;
[p, q, r] = [q, r, p];    // p=2, q=3, r=1

// object assignment (parenthesised so the `{` isn't a block)
var name, age;
({ name, age } = { name: "OS", age: 5 });

// assign straight into object members / array slots
var o = { p: 0, q: 0 };
[o.p, o.q] = [7, 8];

// rest and nesting work here too
[first, ...others] = [1, 2, 3, 4];
[[m], { k: n }] = [[5], { k: 6 }];
```

## How it's implemented

Declaration destructuring (M169) always *creates* bindings (`env_define`).
Assignment destructuring must instead *write to existing targets* — an
identifier resolved up the scope chain, or an object member / array element.
Rather than duplicate the pattern walker, it gained a mode:

- `bind_pat(pattern, value, env, assign)` is the one recursive walker. With
  `assign == 0` (declarations: `var`, parameters, `for-of`) a leaf calls
  `env_define`, exactly as before. With `assign == 1` a leaf instead uses the
  same logic as a normal `N_ASSIGN` target:
  - **identifier** → `env_find` up the chain and overwrite, else define;
  - **member** (`o.p`) → `obj_set` on the receiver;
  - **index** (`a[i]`) → write the array slot / object key.
- Two thin wrappers name the modes: `bind_pattern` (declarations) and
  `bind_pattern_assign` (assignments); both go through the M170 depth guard.
- Parsing is free: `[a, b]` / `{x}` already parse as array/object literals, so
  `[a,b] = …` is just an `N_ASSIGN` whose target is `N_ARRAY`/`N_OBJECT`. The
  `N_ASSIGN` evaluator now routes those targets (for `=`, not compound ops) to
  `bind_pattern_assign`.

Because the leaf handler reuses the member/index assignment paths, targets like
`[o.p, o.q] = …` and `({ x: a.b } = …)` work without extra code. Plain
assignment (`a = 5`, `o.x = 9`, `arr[0] = 7`) is unchanged. Covered by the
regression suite (`tests/js/suite.js`, `make jstest`, ASan/UBSan-clean); the
kernel builds clean.

With this, destructuring is complete across all four positions: `var`
declarations, function/arrow parameters, `for-of` loop variables, and
assignment targets.
