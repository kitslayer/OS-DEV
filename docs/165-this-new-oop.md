# Milestone 165 — `this`, `new`, and constructor-pattern OOP

The JavaScript engine (`kernel/js.c`) gained object-oriented support: the `this`
keyword, the `new` operator, object-literal method shorthand, and lexical `this`
for arrow functions. This is the biggest single capability jump since closures —
it unlocks the whole constructor/method idiom that real scripts lean on.

## What works now

```js
// constructor + methods assigned to `this`
function Counter(start){
    this.n = start;
    this.inc = function(){ this.n++; return this.n; };
}
var c = new Counter(10);
c.inc();          // 11
c.inc();          // 12

// object-literal method shorthand — `this` is the receiver
var account = {
    balance: 100,
    deposit(x){ return this.balance + x; }
};
account.deposit(5);          // 105

// arrow functions inherit `this` lexically (they do NOT rebind it)
function Box(v){
    this.v = v;
    this.scale = function(arr){ return arr.map(x => x * this.v); };
}
new Box(3).scale([1,2,3]);   // [3, 6, 9]   (the arrow sees Box's `this`)

// `new a.b.C(args)` and chaining after `new`
new lib.Make("p").tag();     // member chain binds to new, then a normal call

// a constructor that returns an object overrides the fresh `this`
function Factory(){ this.x = 1; return { x: 99 }; }
new Factory().x;             // 99
```

## How it's implemented

- **`this` binding lives in the call frame.** `this` is dynamic in JS (set by the
  call site, not by where the function was written), so `call_function_this(fn,
  thisv, args, n)` defines `this` as an ordinary variable in the new call
  environment. Member calls (`obj.m()`) pass the receiver; `new` passes the fresh
  object; a plain call (`f()`) passes `undefined`.
- **Arrow functions are marked at parse time** (`node->prefix == 1`) and
  deliberately *skip* defining their own `this`, so a `this` lookup walks up the
  scope chain to the enclosing regular function — exactly JS's lexical-`this`
  rule.
- **`new F(args)`** allocates a fresh object, calls the constructor with `this`
  bound to it, and returns that object unless the constructor explicitly returns
  another object.
- **`new`'s grammar** consumes the member chain (`new a.b.C`) but stops before
  `(` so the parentheses become the constructor's argument list; any further
  `.method()` after the `new` expression chains normally through `parse_postfix`.

## Companion fixes in the same milestone

- **`++` / `--` on members and indices.** `o.n++`, `arr[i]++`, `o[k]++` were
  previously rejected with "invalid ++/-- target" — the update operator only
  resolved bare identifiers. It now resolves member and index lvalues (creating a
  missing property as `0` first, matching JS's numeric coercion of `undefined`).
  This was a pre-existing gap, independent of OOP, but constructors lean on it.
- **`parse_fn_params` helper.** The parameter-list parser (with default values)
  is now shared by `function`, object-literal method shorthand, and constructors
  instead of being duplicated.

## Not yet

`Function.prototype` chains and `class` syntax aren't here yet — methods are
assigned directly to `this` (or live on the object literal), which covers the
common case. Prototype-based inheritance and `class` sugar are the natural next
step. Covered by the regression suite (`tests/js/suite.js`, `make jstest`,
ASan/UBSan-clean).
