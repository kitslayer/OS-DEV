# Milestone 167 — `super` (super-constructor and `super.method()`)

M166 shipped `class`/`extends` but deferred `super`. This milestone completes
single-rooted class inheritance: `super(args)` calls the parent constructor, and
`super.method(args)` calls a parent method — both correctly chaining through
multiple levels of `extends`.

## What works

```js
class Animal {
    constructor(name){ this.name = name; this.legs = 4; }
    speak(){ return this.name + " makes a sound"; }
}
class Dog extends Animal {
    constructor(name){ super(name); this.sound = "woof"; }  // init parent fields
    speak(){ return super.speak() + " (" + this.sound + ")"; }
}
var d = new Dog("Rex");
d.name; d.legs; d.sound;   // "Rex", 4, "woof"   (super() ran Animal's ctor)
d.speak();                 // "Rex makes a sound (woof)"

// chains correctly through three levels
class A { constructor(){ this.tag = "A"; } who(){ return "A"; } }
class B extends A { constructor(){ super(); this.tag += "B"; } who(){ return super.who() + "B"; } }
class C extends B { constructor(){ super(); this.tag += "C"; } who(){ return super.who() + "C"; } }
new C().tag;    // "ABC"
new C().who();  // "ABC"
```

## How it's implemented

Methods are still *copied* onto each instance (the M166 model), so a running
method has no implicit link back to the class that declared it. `super` needs
that link, so each class member records it explicitly:

- **`obj.super_class`** — a new field on a function value's object. When a class
  is evaluated, every *own* method and the *own* constructor store the parent
  constructor there.
- **`@super` call-frame binding.** `call_function_this` binds a hidden `@super`
  variable (the parent constructor, as a function value) into a non-arrow
  function's call frame whenever the function has a `super_class`. Arrow
  functions don't rebind it, so an arrow nested in a method sees the method's
  `super` lexically — matching JS.
- **`super(args)`** resolves `@super` and the current `this`, then calls the
  parent constructor with that `this` (mutating the existing instance in place).
- **`super.m(args)`** looks `m` up in the parent constructor's method table
  (`@super`'s `home_proto`) and calls it with the current `this`. Because the
  parent's method value itself carries the *grandparent* as its `super_class`,
  `super.m()` chains up arbitrarily far.

### The inherited-constructor subtlety

A child with no own `constructor` inherits the parent's constructor *node*. That
node was written inside the parent, so its `super` must mean the **grandparent**,
not the parent — otherwise `super()` inside it would re-invoke the parent and
loop. The class evaluator handles this: an inherited constructor's `super_class`
is set to `parent.super_class` (the grandparent), while an own constructor's is
the direct parent. (This is the exact case the M166 review flagged.)

## Safety

`super` outside a derived constructor (no `@super` in scope) raises a clean,
catchable runtime error rather than crashing — the resolver null-checks the
`@super` lookup and the parent's method table. `extends` cycles can't form (the
class name isn't bound until after its body evaluates), and all `super` call
paths go through the existing `g_depth`/`MAXDEPTH` recursion guard.

Covered by the regression suite (`tests/js/suite.js`, `make jstest`,
ASan/UBSan-clean); the kernel builds clean. With this, the engine supports the
full common shape of class-based JavaScript: constructors, methods, inheritance,
overriding, and `super`.
