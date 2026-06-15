# M261 — Getters & setters (accessor properties)

Added ECMAScript accessor properties to the from-scratch JS engine (`kernel/js.c`):

```js
var o = { _x: 10, get x(){ return this._x; }, set x(v){ this._x = v * 2; } };
o.x;        // 10   — getter fires
o.x = 5;    // setter runs → _x = 10

class Rect {
  constructor(w,h){ this.w = w; this.h = h; }
  get area(){ return this.w * this.h; }   // (new Rect(3,4)).area === 12
  set area(a){ this.w = a; }
}
```

This was the last canonical OOP feature missing, and it had been deferred for a
while as "needs a fresh focused session." It was designed up front by a **Plan
subagent** (which mapped the exact functions/lines to touch) and then built in
small, independently-verified increments.

## The key decision: localized firing, NOT `obj_get`

The obvious place to fire a getter is `obj_get` — it's the one chokepoint every
property read goes through. That is exactly the wrong place:

- `obj_get` is also called by `'k' in o`, `delete o.k`, `for…in`, destructuring,
  `JSON.stringify`, `Object.keys/values/entries`, spread, and `Object.assign`.
  Per spec, **none of those may fire a getter** (`in`/`delete` test existence;
  enumeration reads the slot, not the computed value in our model).
- Firing inside `obj_get` would re-enter the evaluator from deep inside many
  internal call paths, multiplying the re-entrancy / `g_err`-propagation surface.

So instead the accessor is **inert at storage time** and only the *evaluator's*
read/write sites fire it:

| Site | Function | Action |
|------|----------|--------|
| `a.x` read | `eval_member_get` | if value is an accessor → call getter, `this=a` |
| `a[k]` read | `N_INDEX` | same |
| `a.m()` call | `N_CALL` member dispatch | fire getter, then call its result |
| `a.x = v` | `N_ASSIGN` member | if existing value is an accessor → call setter, else `obj_set` |
| `a[k] = v` | `N_ASSIGN` index | same |

`obj_get` keeps returning the accessor **raw**, so `in`/`delete`/enumeration get
the correct non-firing behavior *for free* — no special-casing needed.

## Representation

A new `obj->kind` value `V_ACCESSOR`, stored **as the property value** (the
`val.t` stays `V_OBJ`, exactly like the existing `V_BOUND` bound-function trick),
with the getter in `vals[0]` and the setter in `vals[1]` (`UND()` when absent).
Because `obj_keyed()` only recognizes `V_OBJ`/`V_REGEX`, an accessor object is
never walked as a key/value bag — it is opaque everywhere except the five sites
above.

## Parsing

`get`/`set` are **not** reserved words (they must remain usable as ordinary
property names and method names). The object-literal and class-body parsers use
`lex_save`/`lex_restore` one-token lookahead: only `get`/`set` followed by a
property name **and** `(` is an accessor; everything else restores the lexer and
falls through unchanged. So `{get:1}`, `{get(){}}`, `{get,set}`, and a field/
method named `get` all still parse identically.

## Safety (256 KB guard-page-less stacks)

Getter/setter bodies re-enter only through the depth-guarded
`call_function_this`, so a self-referential accessor like
`get x(){ return this.x; }` hits `MAXDEPTH` (120) and throws cleanly rather than
overflowing the stack. Verified under ASan/UBSan along with the localized
invariant (a getter with a side effect is **not** triggered by `in`, `delete`,
or `JSON.stringify`).

## v1 limitations (documented, not bugs)

- `JSON.stringify` / `Object.values` / `Object.entries` / spread `{...o}` /
  `Object.assign` iterate `vals[]` directly, bypassing the eval read sites, so
  they do **not** fire getters (an accessor serializes as `{}`).
- `static get`/`static set` on a class is parsed but not wired up.
- `super.<accessor>` (reading an inherited accessor through `super`) reads the
  raw parent slot and does not fire — normal inherited access through an
  instance fires correctly.

(`o.x++` / `o.x += 1` *do* fire correctly — `+=` always did; `++`/`--` were
routed through the getter+setter in the M261 review follow-up, after the review
found that `o.x++` had been silently overwriting the accessor with a plain
number instead of leaving it intact.)

## Reusable lesson

When adding a feature that *seems* to require intercepting a ubiquitous
chokepoint, check whether a **localized** set of evaluator sites gives the same
observable behavior. Here it was both simpler and safer: it kept `in`/`delete`
non-firing automatically and avoided turning `obj_get` into a re-entrant call
site. A Plan subagent enumerating the exact sites made it tractable mid-session
instead of a risky open-ended rewrite.

Verified: host ASan/UBSan (all cases incl. infinite-recursion, in/delete
non-firing, JSON limitation, class inheritance), `make jstest` golden (suite
lines 174–184), clean kernel build, and in-OS (`js -e` returns the getter value
in ring-3 userspace).
