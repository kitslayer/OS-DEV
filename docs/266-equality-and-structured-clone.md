# M266 — structuredClone, and a fundamental `===` bug

## structuredClone

`structuredClone(v)` is a deep clone that **preserves cycles and shared
references** — the thing the usual `JSON.parse(JSON.stringify(v))` workaround
cannot do:

```js
var shared = { v: 1 };
var src = { a: shared, b: shared };
var c = structuredClone(src);
c.a === c.b;        // true  — both point to the ONE clone of `shared`
src.a === c.a;      // false — the clone is independent of the source

var cyc = {}; cyc.self = cyc;
var cc = structuredClone(cyc);
cc.self === cc;     // true  — the cycle is reproduced, and it didn't hang
```

Implementation: a recursive `sclone(v, seen, …)`. Primitives are copied by value;
plain objects and arrays are cloned recursively; a **`seen` map** (a bounded
array of `{from, to}` pairs) sends a re-encountered source object to the *same*
clone, which is what reproduces cycles and shared refs. Functions and the exotic
kinds (Map/Set/Date/RegExp/accessor) throw, per spec. It is depth-capped (64),
and `seen[]` lives in the caller's frame, so there is no per-level allocation and
no way for untrusted input to overflow the 256 KB guard-page-less stack.

## The bug it surfaced: `===` compared all objects equal

Writing the structuredClone test (`src.a === c.a` should be `false`) exposed a
**pre-existing** correctness bug in the engine's `===` / `!==` (and `==` / `!=`)
operators:

```js
{} === {}           // was TRUE  (should be false)
var a = {}, b = {};
a === b;            // was TRUE
[1] === [1];        // was TRUE
```

The cause: after the string and type-mismatch checks, the comparison fell
through to `to_num(a) == to_num(b)`. `to_num` coerces **every** object to `0`, so
*any two objects compared equal*. The fix is one line per operator: when the
operands are objects/arrays/functions, compare by **reference identity**
(`a.o == b.o`) instead. Primitive comparison (strings, numbers, booleans,
null/undefined) was already correct and is unchanged.

### Why it hid for so long, and the lesson

`Object.is` (added in M259) was *correct* for objects the whole time — because it
delegates to `val_equal`, which does identity. The `===` **operator** had its own,
separate, buggy code path that nobody had cross-checked against `val_equal`. So
the engine simultaneously had a correct identity comparison (`Object.is`) and a
broken one (`===`).

The lesson, now recorded in the project memory: **when you add one form of a
comparison, test the *other* forms against it — and always test `===` on two
*distinct* objects, not just `a === a`.** Same-reference and primitive tests
(the easy ones to write) both passed, which is exactly why the bug survived. It
took comparing a clone against its source — two structurally-equal but distinct
objects — to reveal it.

Verified: host ASan/UBSan (deep clone independence, shared-ref + cycle
preservation, function/Map/Set throw; object identity in `===`/`!==`, same-ref
true, primitives unchanged) + `make jstest` (the suite never relied on the bug,
so it passed unchanged) + clean kernel build.
