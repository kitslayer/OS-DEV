# M263 — Prototype chain (additive)

Added prototype-based OOP to the from-scratch JS engine — the paradigm a huge
amount of library/legacy/transpiled web JS relies on:

```js
var o = Object.create({ greet(){ return "hi " + this.name; } });
o.name = "Ada";  o.greet();                 // "hi Ada"  (inherited method)

function F(){ this.a = 1; }
F.prototype.m = function(){ return this.a + 1; };
new F().m();                                 // 2

o.__proto__;  Object.getPrototypeOf(o);  Object.setPrototypeOf(o, p);
```

The engine's classes already worked (M165+) by **copying** methods onto each
instance (`obj.home_proto`), not via a shared prototype. M263 adds a real
`[[Prototype]]` chain **without touching that** — and without touching the
hottest path's behavior for the millions of objects that have no prototype.

## The design: additive, eval-site-only

Two struct fields:
- `obj *proto` — an object's chain parent. `new_obj` memsets it to NULL, so
  **every object that existed before M263 has no proto** — the new lookup is a
  single non-taken `if (recv.o->proto)` branch for them.
- `obj *fn_proto` — a plain function's `.prototype` object. (It lives in a
  *field*, not a keyed property — see the gotcha below.)

The chain is walked by one helper, **only at the evaluator's member sites**,
and only *after* an own-property lookup misses:

| Site | What |
|------|------|
| `eval_member_get` (`a.x`) | own miss → `proto_lookup` walks the chain |
| `N_INDEX` (`a[k]`) | same |
| `N_CALL` dispatch (`a.m()`) | own miss → walk, call inherited method with `this`=`a` |
| `N_ASSIGN` (`a.x = v`) | inherited **setter** fires; else own data prop (shadow) |

```c
static int proto_lookup(obj *start, const char *name, val recv, val *out) {
    int guard=0;
    for (obj *p=start; p && ++guard<=JS_PROTO_MAX; p=p->proto)
        if (obj_get(p,name,&out_v)) { if (is_accessor) *out=fire_getter(.., recv); ... return 1; }
    return 0;
}
```

An inherited **accessor** (from M261) fires with `this` = the *original*
receiver, not the prototype that holds it.

## Why this is safe — the own-only invariant

The walk lives **only** in those eval sites; **`obj_get` itself is never
changed**. Everything that reads properties by calling `obj_get` directly or by
iterating `keys[]` therefore stays own-only with **zero edits**:

- `delete o.x`, `'x' in o`, `for…in`, `Object.keys/values/entries`,
  `Object.assign`, `JSON.stringify`, object destructuring `{x} = o`

This is exactly the spec-correct behavior for most of them (e.g. `delete` and
enumeration are own-only) and it means a getter with a side effect on a
prototype is **never** triggered by those operations.

## Safety details

- **Cycle cap.** `a.__proto__=b; b.__proto__=a` is legal to *construct*; the walk
  is bounded by `JS_PROTO_MAX` (1000), so a missing-property lookup on a cyclic
  chain terminates returning `undefined` instead of hanging. `proto_lookup` is
  iterative — no recursion, no stack growth.
- **Re-entrancy.** Inherited getters/setters/methods fire via the
  depth-guarded `call_function_this`, so `Object.create({get x(){return this.x}}).x`
  hits `MAXDEPTH` and throws rather than overflowing the 256 KB guard-page-less
  stack.
- **Classes unchanged.** At `new`, `home_proto == NULL` distinguishes a plain
  function from a class constructor; only plain functions get their instance's
  `proto` linked to `F.prototype`. Class instances are built byte-identically to
  before (the suite's class/super/getter/setter coverage is the backstop).

## The gotcha that bit during implementation

The Plan assumed `F.prototype` could be stored as a keyed property on the
function via `obj_set(F, "prototype", …)`. It can't: `obj_keyed()` is only
`V_OBJ`/`V_REGEX`, so **`obj_set` on a `V_FUN` is a silent no-op**. The symptom
was subtle — `F.prototype.m = …` appeared to work (each access returned a
*fresh* object), but `new F()` then linked instances to a *different*, empty
prototype, so `f.m()` was "no such method". Fix: store `.prototype` in the
dedicated `fn_proto` field. Lesson: on this engine, only `V_OBJ`/`V_REGEX` hold
keyed properties — function/native side-data needs a struct field (like
`statics`, `home_proto`, and now `fn_proto`).

## v1 scope

In: `Object.create(proto)`, `getPrototypeOf`, `setPrototypeOf`, function
`.prototype`, `new F()` for plain constructors, `__proto__` get/set, inherited
read/call/accessor-write, own-shadows-inherited.

Deferred (documented): the `in` operator walking the chain, `Object.create`'s
2nd descriptor argument, index-form `o["__proto__"]`, and `instanceof` consulting
`.prototype` (it still uses the independent `ctor_class`/`parent_class` walk).

## Reusable lesson

To touch the hottest path safely: make the change **additive** (a new branch
gated behind a field that is NULL for all existing data), keep the shared
primitive (`obj_get`) semantically unchanged so unrelated callers are
automatically correct, bound any new loop, and let a Plan subagent map the exact
sites + a review subagent gate it. That combination made a member-lookup change
shippable mid-session with the class system provably intact.

Verified: host ASan/UBSan (Object.create, plain-ctor `.prototype`, own-shadows-
inherited with the base untouched, `__proto__` read/write, inherited accessor
firing with `this`=instance, `__proto__` cycle terminating, class regression) +
`make jstest` golden (suite lines 188–196) + clean kernel build + in-OS
(`Object.create({m(){return 7}}).m()` → 7 in ring-3 `js`).
