# Milestone 166 — `class` syntax (with `extends`)

Building on M165's `this`/`new`, the JavaScript engine now parses and runs
`class` declarations and expressions, including single-level `extends`
inheritance. Combined with the constructor pattern, this covers the great
majority of object-oriented JavaScript that demo and tutorial code uses.

## What works

```js
class Animal {
    constructor(name){ this.name = name; }
    speak(){ return this.name + " makes a sound"; }
    describe(){ return "I am " + this.name; }
}

class Dog extends Animal {
    speak(){ return this.name + " barks"; }   // override
}

var d = new Dog("Rex");
d.speak();      // "Rex barks"      (own method overrides the parent's)
d.describe();   // "I am Rex"        (method inherited from Animal)
d.name;         // "Rex"            (Dog has no constructor -> Animal's runs)

// methods can call sibling methods through `this`, and return `this` to chain
class Acc {
    constructor(){ this.t = 0; }
    add(x){ this.t += x; return this; }
}
new Acc().add(3).add(4).t;   // 7

// classes are also expressions
var Pt = class { constructor(x){ this.x = x; } get(){ return this.x; } };
new Pt(42).get();            // 42
```

## How it's implemented — method copying, not a prototype chain

A real JS engine puts methods on `Constructor.prototype` and resolves
`obj.method` by walking the prototype chain. This engine takes a simpler route
that fits its arena model and needs no changes to property lookup or method
dispatch:

- Evaluating a `class` builds a **method table** object `P` (one entry per
  method, each a function value closed over the class's scope) and a
  **constructor function value** that carries `P` in a new `home_proto` field.
- `new C(args)` allocates the instance and **copies every method from
  `home_proto` onto it as an own property**, then runs the constructor with
  `this` bound to the instance.

Because methods become ordinary own properties, the existing `obj.method()`
dispatch and `this`-binding (M165) work unchanged. The cost is one shared
function value copied per method per instance — cheap, since the function
objects themselves are shared (only the small `val` is copied).

### `extends`

At class-evaluation time, `extends Parent` copies the parent's method table into
the child's first, so child methods added afterwards naturally **override**
parent methods of the same name. If the child declares no `constructor`, it
**inherits the parent's** constructor node, so `new Child(...)` still initializes
the parent's fields.

## Not yet: `super`

`super(...)` / `super.method(...)` are not implemented — `super` currently
resolves as an undefined variable and raises a clean catchable error rather than
crashing. Wiring `super` (binding the current method's home class at call time)
is the next increment. Multi-level field initialization through an explicit
child constructor therefore needs `super` and isn't supported yet; everything
else above is.

Covered by the regression suite (`tests/js/suite.js`, `make jstest`,
ASan/UBSan-clean); the kernel builds clean.
