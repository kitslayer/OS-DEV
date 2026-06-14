# Milestone 191 — fix CRITICAL Date/Map/Set `keys[]` crash (review)

The M185–189 review found a **CRITICAL, remotely-triggerable kernel crash**:
reading an absent property of a `Date` (or `Map`/`Set`), `JSON.stringify`-ing
one, `Object.keys`/`for…in` over one, or spreading one — all from untrusted
page JS — dereferenced a garbage pointer and SEGV'd the kernel (256 KB stack,
**no guard page**).

```js
new Date().getTime;          // feature-detect — CRASH
JSON.stringify(new Date());  // CRASH
Object.keys(new Date());     // CRASH
for (var k in new Date()) {} // CRASH
JSON.stringify(new Map().set("a",1));  // same hazard
```

## Root cause

`Date`/`Map`/`Set` (and arrays) are `V_OBJ` values that store their data in the
object's `vals[]` via `arr_push_val`. But `arr_push_val` grows **only** `vals[]`,
never `keys[]` — and `aalloc` does not zero memory. So for such an object,
`keys[]` is **uninitialised garbage** *and shorter than `n`*. Any code that
iterates `keys[i]` as property keys — `obj_get`/`obj_set`, `JSON.stringify`,
`Object.keys`/`values`/`entries`, `for…in`, object spread/rest, `Object.assign`,
`o.x++` — read a garbage/out-of-bounds pointer and `strcmp`'d it → crash. A
`Date` made this routine: it's *never* empty (always 6 components) and
`date.<x>` / `JSON.stringify(date)` are common in real code.

(An initial "skip NULL keys" attempt was wrong — the keys aren't NULL, they're
garbage. ASan caught it immediately.)

## Fix — gate every `keys[]` iteration on the object kind

A new predicate distinguishes objects with real keyed properties from
data-in-`vals[]` objects:

```c
/* V_OBJ and V_REGEX use obj_set -> real keys[]; V_ARR/V_MAP/V_SET/V_DATE use
 * arr_push_val -> keys[] is garbage and must never be iterated as keys. */
static int obj_keyed(obj *o){ return o && (o->kind==V_OBJ || o->kind==V_REGEX); }
```

`obj_get`/`obj_set` return early for non-keyed objects; `JSON.stringify`,
`Object.keys`/`values`/`entries`, `for…in`, object spread/rest, `Object.assign`,
and the `o.x++` member/index update paths all gate their key iteration on
`obj_keyed`. As a bonus, `JSON.stringify(date)` now serializes as the date
string, and `JSON.stringify(map/set)` as `{}` — matching JS, where their data
isn't own-enumerable.

(`V_REGEX` is unaffected: it sets `source`/`global`/`flags` via `obj_set`, so its
`keys[]` is valid — which is why the regex review never hit this.)

## Verified

All PoCs now return cleanly (`date.foo`→`undefined`, `JSON.stringify(date)`→the
quoted date string, `Object.keys(date)`→`[]`, `{...set}`→`{}`); normal objects
are fully intact (`{a:1}` keys/values/stringify/access all correct). Regression
coverage for these introspection patterns was added to `tests/js/suite.js`, and
the suite — exercising them — **runs to completion in the real kernel** with the
OS staying responsive. `make jstest` passes ASan/UBSan-clean (its M190
completion-guard confirms a full run); the kernel builds clean.

**Lesson (in memory):** any object that stores data via `arr_push_val` must
never have its `keys[]` treated as property keys — gate on the kind.
