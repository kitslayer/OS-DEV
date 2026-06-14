# Milestone 171 — parameter destructuring

The destructuring patterns from M169 now work as **function, method, and arrow
parameters** — the form real code uses constantly for object "options" arguments
and array-callback unpacking.

## What works

```js
// object-destructuring parameter, with a default
function greet({ name, age = 0 }){ return name + " (" + age + ")"; }
greet({ name: "Ada", age: 36 });   // "Ada (36)"
greet({ name: "Bo" });             // "Bo (0)"

// array-destructuring parameter
function dist([x, y]){ return x*x + y*y; }
dist([3, 4]);                      // 25

// the everyday array-callback case
[[1,2],[3,4],[5,6]].map(([a, b]) => a + b);   // [3, 7, 11]
[{n:"x",v:1},{n:"y",v:2}].map(({ n, v }) => n + "=" + v);  // ["x=1","y=2"]

// rest, nesting, mixed, and a whole-parameter default all work
function head([first, ...others]){ return first + "/" + others.length; }
function f({ pos: [px, py], tag }){ return tag + ":" + px + "," + py; }
var g = (x, [y, z]) => x + y + z;            // mixed ident + pattern params
function opt({ verbose = false } = {}){ return verbose; }   // default when no arg
```

## How it's implemented

Parameters are fresh bindings created in the new call frame, so they have the
exact same semantics as a `var` destructuring declaration — which means the
M169 `bind_pattern` is reused verbatim:

- **`parse_fn_params`** (shared by `function`, methods, and class members) now
  accepts a `[`/`{` parameter: it parses the pattern with the array/object
  literal parser, and an optional `= default` wraps it in an `N_ASSIGN` (so a
  whole-parameter default applies when the argument is omitted).
- **The arrow-detection lookahead** in `parse_assign` learned the same: inside
  the speculative `( … ) =>` scan it accepts `[`/`{` pattern parameters. Because
  the scan is bounded by `lex_save`/`lex_restore`, a *non-arrow* parenthesized
  expression like `([1,2,3])` or `({x:5})` is still re-parsed correctly as a
  literal when no `=>` follows — verified.
- **`call_function_this`** routes any non-`N_IDENT` parameter node to
  `bind_pattern` (with the i-th argument, or `undefined` when missing); plain
  identifier params and `...rest` keep their existing fast paths.

`bind_pattern` carries its own depth guard (M170), so nothing here can overflow
the C stack. Covered by the regression suite (`tests/js/suite.js`, `make jstest`,
ASan/UBSan-clean); the kernel builds clean.

## Not yet

Assignment-target destructuring (`[a, b] = [b, a]` / `({x} = o)` without a
declaration) still raises "invalid assignment target" — it needs assignment
(not declaration) semantics for the leaves, so it's a separate, careful step.
