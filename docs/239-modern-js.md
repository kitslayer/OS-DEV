# Milestones 239–244 — modern JavaScript syntax

After completing the operator set (M228–232) and assignment/number-literal
sets (M233–234), this arc closed the gap on several syntactic features that
real, modern JavaScript uses constantly. They were found the efficient way:
**systematic probing** — feeding candidate snippets through the host build to
see what throws `[js error: syntax...]`, then fixing each genuine gap. Five
surfaced; all are now supported. (The probe also *confirmed* a large set of
features already work — object spread, sort-with-comparator, deep
destructuring, string spread, chained array methods, nested JSON, default
parameters, trailing commas, optional chaining including `a?.[k]` — so they
needn't be re-investigated.)

Each milestone followed the standard loop: host AddressSanitizer/UBSan test →
a `tests/js/suite.js` line + regenerated golden → `make jstest` → a clean
kernel build → an in-OS check (often a short `js -e` one-liner, which fits the
terminal where the long suite scrolls off). Two review subagents covered the
batch (M238–240 and M241–243); both returned SHIP.

## M239 — logical assignment `||=`, `&&=`, `??=`

These assign *conditionally* and **short-circuit** the right-hand side:
`a ||= b` assigns only if `a` is falsy, `a &&= b` only if truthy, `a ??= b`
only if `a` is `null`/`undefined`. Crucially the RHS is not evaluated when it
won't be assigned (`x ||= sideEffect()` never calls `sideEffect()` if `x` is
truthy) — so `N_ASSIGN` was restructured: the logical branch evaluates the
target, checks the condition, and returns early *before* touching the RHS; the
existing `=`/`+=`/… path is unchanged in the `else`.

A subtle footgun: the three characters `??=` are the C **trigraph** for `#`.
GCC ignores trigraphs by default but warns, and it's fragile, so the literal is
written `"?\?="` — the backslash breaks the trigraph while the string still
denotes `?`, `?`, `=`.

## M240 — public class fields

`class C { count = 0; }` — instance fields with optional initializers. The
parser distinguishes a member followed by `(` (a method) from one followed by
`=`/`;`/end (a field), and synthesizes a `this.name = init` assignment into a
per-class field block. Each class constructor stores its *own* fields
(`co->fields`, a new struct field that defaults to `NULL`, so classes without
fields are completely unaffected). `new` runs the field initializers **up the
parent chain, parent-first**, with `this` bound, *before* the constructor body
— so inherited fields work and a subclass can override. Field initializers are
ordinary statements run through the existing machinery, so there is no change
to the evaluator core. (`static` members are parsed as instance members for
now; true class-level statics are a follow-up.)

## M241 — `typeof` on an undeclared variable

`typeof somethingNeverDeclared` now yields `"undefined"` instead of throwing
"undefined variable". This is the universal feature-detection idiom
(`typeof module !== "undefined"`, `typeof window !== "undefined"`) and real
scripts rely on it. The fix is a single special case in the `typeof` branch:
if the operand is a bare identifier with no binding, return `"undefined"`
without evaluating it. Only a bare identifier is guarded —
`typeof undeclaredObj.prop` still throws, matching the language.

## M242 — numeric separators

`1_000_000`, `0xFF_FF`, `0b1010_1010`, `0o7_5_5` — an underscore may sit
between digits in any base. Each base's digit-scanning loop simply skips `_`
without folding it into the value. A leading underscore still begins an
identifier (the number scanner only starts on a digit), and underscores inside
identifier names are untouched.

## M243 — computed method names

`{ [expr](){ … } }` — an object literal may use a computed key for a method,
not just for a value (`{ [expr]: v }` already worked). The object-literal
parser now accepts `(` after the `[expr]` key and parses a method there. No
evaluator change was needed: the method's function node is stored under the
computed key and instantiated by the existing computed-key path, so it gets a
dynamic `this` like any object method.

## M244 — static class members

`static method(){…}` and `static field = …` — members that live on the class
itself (`MathUtil.square(5)`, `Config.DEFAULT`) rather than on instances. This
completes the modern class story alongside M240's instance fields.

The constructor is a function value (`V_FUN`), and functions don't carry keyed
properties in this engine, so statics are kept in a **side object**: the parser
routes `static` members into their own block, and the class evaluator builds
them into a fresh `V_OBJ` hung off the constructor (`co->statics`), evaluated
after the class name is bound so a static can reference its own class. Two
narrowly-gated lookups consult it — one in member-read for `Class.field`, one in
the call dispatch (after the `.call`/`.apply`/`.bind` cases) so `Class.method()`
invokes with `this` bound to the class, which makes `static make(){ return new
this(); }` work. Both branches are guarded on `recv` being a function with a
non-null `statics`, so regular functions and classes without statics are
completely unaffected. (Static *inheritance* — a subclass seeing a parent's
statics — is a deliberate follow-up.)

## Where this leaves the engine

With these in place the engine handles essentially all of the everyday modern
syntax. What remains is either architectural (getters/setters, a real DOM tree,
generators/iterators, a persistent per-page environment for `addEventListener`,
class-level statics) or deliberately niche (labeled break/continue, tagged
templates) — see the project notes.
