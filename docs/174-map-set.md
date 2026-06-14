# Milestone 174 — `Map` and `Set`

The two core ES6 collections, with arbitrary (non-string, identity-compared) keys
and full iteration — the highest-value data-structure gap in the engine.

## What works

```js
// Map: any key type, insertion order, chainable set()
var m = new Map();
m.set("a", 1).set("b", 2).set("a", 9);   // overwrite
m.size; m.get("a"); m.has("b"); m.get("z");   // 2, 9, true, undefined
m.delete("b");

// keys are compared with === — numbers, booleans and objects stay distinct
var k = { id: 1 };
m.set(1, "num").set(true, "bool").set(k, "obj");
m.get(1);        // "num"   (1 !== true)
m.get({id:1});   // undefined  (different object identity)

m.forEach((v, key) => …);
for (var [key, val] of m) …;
m.keys(); m.values(); m.entries();

// Set: deduped, insertion order
var s = new Set();
[3,1,3,2,1].forEach(x => s.add(x));
s.size;                    // 3
s.has(2);                  // true
for (var v of s) …;        // 3, 1, 2
[...s];                    // dedupe an array in one line
```

## How it's implemented

- A `Map`/`Set` is an ordinary `V_OBJ` value whose `obj->kind` is a new `V_MAP`
  or `V_SET` marker, so `typeof` is `"object"` and it flows through the engine
  normally. Entries live in the object's existing `vals` array — a Map stores
  them interleaved `[k0, v0, k1, v1, …]` (size = `n/2`), a Set stores `[v0, v1,
  …]`. Lookup is a linear scan using a new `val_equal` helper implementing `===`
  (primitives by value with **strict** type match — `1 !== true` — objects by
  pointer identity).
- `new Map()` / `new Set()` work because `new` now accepts a **native
  constructor**: when the constructee is a native function, `new` calls it and
  uses its return value as the instance. `Map`/`Set` are registered as native
  globals whose function builds the empty collection.
- Methods dispatch from the `N_CALL` method path: a `V_MAP`/`V_SET` receiver
  routes to `eval_map_method` / `eval_set_method` (set/get/has/delete/clear/
  forEach/keys/values/entries for Map; add/has/delete/clear/forEach/values for
  Set). `.size` is read in `eval_member_get`. `for-of` iterates a Set's values
  and a Map's `[key, value]` pairs (a fresh 2-element array per entry).

All allocations are OOM-guarded (`new_obj`/`arr_push_val` bail on `g_oom`).
Covered by the regression suite (`tests/js/suite.js`, `make jstest`,
ASan/UBSan-clean); the kernel builds clean.

## Notes / limits

Lookup is O(n) per operation (linear scan), fine for the small collections
typical of page scripts; there's no hashing. `WeakMap`/`WeakSet` and the
iterator-protocol objects returned by `keys()`/`values()` in real JS are
simplified to plain arrays here.
