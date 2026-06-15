# Milestones 228–231 — completing the JavaScript operator set

By M227 the from-scratch JavaScript engine (`kernel/js.c`) had classes, ES6,
regex, `Map`/`Set`, `Date`, and a comprehensive standard library — but it was
still missing several *operators* that real code reaches for constantly. This
arc filled that gap: `delete`, `in`, bitwise `^`/`~`, and `instanceof`. Each is
small, but together they remove a class of "why doesn't this parse?" surprises.

Every operator was verified the same way: a host build under
AddressSanitizer + UndefinedBehaviorSanitizer
(`gcc -DJS_HOSTTEST -O1 -fsanitize=address,undefined …`) on a 256 KB-limited
stack (matching the kernel's guard-page-less task stack), a new section added to
the `tests/js/suite.js` regression suite with a regenerated golden, `make jstest`
(which fails on any `[js error:` or a run that doesn't reach `-- done --`), a
clean kernel build, and an in-OS boot running `js suite.js` to completion.

## M228 — the `delete` operator

`delete obj.x` and `delete obj[k]` remove an own property; `delete arr[i]` clears
an array slot to `undefined`. The engine stores object properties in parallel
`keys[]`/`vals[]` arrays, so `obj_delete()` simply finds the key and shifts the
tail down — exactly the proven shift already used by `Map.prototype.delete` /
`Set.prototype.delete`.

The important design choice is that `delete` does **not** touch the hot
`obj_get`/`obj_set` paths. It is a new `N_UNARY` case (op `'d'`) that
special-cases its operand *before* evaluating it: for an `N_MEMBER` or `N_INDEX`
node it evaluates the *object* sub-expression and takes the key, rather than
evaluating the member access to a value. (The `typeof` operator already uses this
"look at the operand node, don't just evaluate it" pattern.)

Making `delete` a keyword raised one regression risk: would `map.delete(k)` still
parse as a method call? It does — the member parser at the `.` case interns the
*raw token text* of whatever follows the dot, so a keyword token named `delete`
becomes the property name `"delete"` just like an identifier would. Verified
directly in the suite.

## M229 — the `in` operator

`key in obj` tests own-property existence (so `"c" in {c:0}` is `true` —
existence, not truthiness); `i in arr` tests for a valid index. It lives at
relational precedence (8), implemented as `N_BINARY` op `'I'`, with the lookup
routed through `obj_get` for objects and a bounds check for arrays.

Two parser details made this more than a one-liner:

- `bin_prec()` only classified `T_PUNC` tokens; `in` is a keyword, so an explicit
  `if (t.type==T_KW && tok_is(t,"in"))` branch was added *before* the
  `if (t.type!=T_PUNC) return 0;` guard.
- The `for (x in obj)` loop detection matched a `T_IDENT` named `in`. Now that
  `in` lexes as `T_KW`, that check was widened to accept either token type, so
  for-in still parses. (`of` stays a contextual identifier, so for-of is
  unaffected.)

## M230 — bitwise `^` and `~`

The bitwise set had `&`, `|`, `<<`, `>>` but was missing XOR and complement.
`^` slots into `bin_prec` at precedence 5 — correctly *between* `&` (6) and `|`
(4), so `1 | 2 ^ 3 & 3` parses as `1 | (2 ^ (3 & 3))`. `~` joins the unary
operators alongside `!`/`-`/`+`. The lexer already emitted both as single-char
`T_PUNC` tokens (via its single-character fallback), so no lexer change was
needed. Integer-only `Number` makes the C `^`/`~` semantics a direct match.

## M231 — the `instanceof` operator

This one needed a small change to the object model. The engine deliberately has
**no prototype chain**: `new Foo()` *copies* the class's methods onto a plain
object. That means an instance kept no link back to its constructor, so there was
nothing for `instanceof` to consult.

Two `obj` fields were added (both default to `NULL` because `new_obj` memsets the
struct, so a plain object literal is `instanceof` nothing — correct):

- `ctor_class` — set in `N_NEW` to the constructor that built the instance.
- `parent_class` — set on each class constructor to its **true direct parent**.

`instanceof` then walks `instance.ctor_class → parent_class → …` looking for the
right-hand constructor. The subtlety is *why `parent_class` is separate from the
existing `super_class`*: for a class that inherits its constructor (no own
`constructor`), `super_class` points at the **grandparent** so that `super()`
resolves to the right place. Reusing it for `instanceof` would make the chain
skip the direct parent — so a dedicated, always-the-direct-parent pointer was the
clean fix. Verified with single-level and inherited classes, and that
`instanceof` returns `false` for plain objects, primitives, arrays, native
constructors (`Map`), and non-constructor right-hand sides.

## Why these were safe to add late in the engine's life

All five are additive: four are brand-new keyword/operator code paths, and
`instanceof`'s two struct fields are only read by `instanceof` itself and written
at construction. None alter the existing `obj_get`/`obj_set`/property-access hot
paths, which is what keeps the blast radius small on an engine that parses
untrusted page JavaScript on a kernel stack with no guard page.
