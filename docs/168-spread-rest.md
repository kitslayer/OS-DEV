# Milestone 168 — spread and rest (`...`)

The `...` operator now works everywhere modern JavaScript uses it: spreading
arrays/strings into array literals and calls, spreading objects into object
literals, and gathering trailing arguments into a rest parameter.

## What works

```js
// spread in array literals (arrays and strings)
var a = [1, 2, 3];
[0, ...a, 4];        // [0, 1, 2, 3, 4]
[...a, ...a];        // [1, 2, 3, 1, 2, 3]
[..."hi"];           // ["h", "i"]

// spread in calls — including built-ins and constructors
function sum3(x, y, z){ return x + y + z; }
sum3(...a);              // 6
sum3(10, ...[20, 30]);   // 60
Math.max(...[5, 9, 2]);  // 9
new Point(...[7, 8]);    // Point{x:7, y:8}

// rest parameters (regular functions AND arrows)
function collect(first, ...rest){ return first + "|" + rest.join(","); }
collect(1, 2, 3, 4);     // "1|2,3,4"
var addAll = (...nums) => nums.reduce((s, n) => s + n, 0);
addAll(1, 2, 3, 4, 5);   // 15

// object spread (later keys win)
var base = { a: 1, b: 2 };
({ ...base, b: 20, c: 3 });   // { a: 1, b: 20, c: 3 }
```

## How it's implemented

- A new `N_SPREAD` AST node wraps the spread expression. `parse_list` (shared by
  array literals and call arguments) and the object-literal parser both detect a
  leading `...` and emit one.
- **Array literals** (`N_ARRAY` eval) expand a spread element in place: array
  elements are appended one by one; a string spreads to its characters.
- **Object literals** (`N_OBJECT` eval) copy every own key/value of a spread
  object; a later key overrides an earlier one (standard `obj_set` semantics).
- **Calls** go through a new `build_args` helper that flattens the argument
  nodes — expanding spreads — into the fixed 16-slot argument array (used by
  both `N_CALL` and `new`). Spreading more than 16 effective arguments is
  capped, matching the engine's existing 16-argument limit.
- **Rest parameters** are marked at parse time (`param->op == '.'`); when a
  function is called, the rest parameter gathers `args[i..]` into a fresh array.
  Both `function`/method params and arrow params support `...rest` — the arrow
  detection lookahead was taught to accept a leading `...`.

All paths are NULL/OOM-guarded (spreads use `arr_push_val`/`obj_set`, which bail
on arena exhaustion). Covered by the regression suite (`tests/js/suite.js`,
`make jstest`, ASan/UBSan-clean); the kernel builds clean.
