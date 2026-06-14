# Milestone 144 — a from-scratch JavaScript interpreter

**Goal:** give the OS a real scripting language — a JavaScript interpreter written
from scratch, runnable from the shell, as the foundation for in-page scripting in
the browser (milestone 145).

## What

`kernel/js.c` (~600 lines) is a **tree-walking JavaScript interpreter**:

- **Lexer** — numbers (decimal + hex), strings (with escapes), identifiers,
  keywords, comments (`//` and `/* */`), and multi-char operators.
- **Parser** — recursive descent + precedence-climbing (Pratt) for expressions,
  producing an AST. Statements: `var`/`let`/`const`, `if`/`else`, `while`, `for`,
  blocks, `return`, `break`, `continue`, function declarations.
- **Evaluator** — tree-walk with lexical-scope environments and **closures**;
  values are a tagged union (undefined, null, boolean, number, string, object,
  array, function, native).
- **Language**: arithmetic + comparison + logical + bitwise operators, `?:`,
  `++`/`--`, `typeof`, string concatenation, **functions/closures/recursion**,
  **arrays** (literals, `[i]`, `.length`, `push`/`pop`/`join`/`indexOf`),
  **objects** (literals, `.prop`, `obj[key]`), string methods
  (`toUpperCase`/`toLowerCase`/`substring`/`slice`/`charAt`/`charCodeAt`/`indexOf`),
  and the builtins `print()` / `console.log()`.

## Deliberate simplifications (it's a kernel with no FPU and tiny stacks)

- **`Number` is a 64-bit integer.** The kernel is built `-mgeneral-regs-only`
  (no FPU/SSE), so there are no floats — `7/2 === 3`. Real doubles would need a
  soft-float library.
- **Arena allocator, reset per run** — no garbage collector. One script runs,
  prints, and the 1 MB arena is recycled. Bounded by `JS_ARENA`.
- **Recursion is depth-limited** (`MAXDEPTH`) so a pathological script can't blow
  the guard-page-less kernel stack.

## How to run it

- `js` — runs a built-in demo (recursion, arrays, objects, bit-ops).
- `js sample.js` — runs a script from the FAT32 disk (`SAMPLE.JS` is baked on).
- `edit foo.js` then `js foo.js` — write and run your own.

Wiring: the ring-3 shell calls **`SYS_js`** → `js_run()` in the kernel (on the
app's 256 KB kernel stack), output captured back to the shell.

## Verified

Host-tested (`-DJS_HOSTTEST`) against a 13-case suite (arithmetic precedence,
closures, fib/factorial recursion, arrays, objects, string methods, `typeof`,
ternary) — all byte-exact. Runs live in the OS via `js` / `js sample.js`.

## Hardened after review

A security review (the interpreter executes **untrusted** script input) found and
fixed **5 critical memory-safety bugs**, all sanitizer-verified:
- `aalloc` made 64-bit / bounds-checked (a count whose `sizeof*n` overflowed 32-bit
  was truncating to a tiny/zero size → OOB write).
- Parser **and** evaluator recursion guarded by `MAXDEPTH` (deep `!!!!`/`(((`/
  member chains / self-referential values previously overflowed the stack).
- `val_to_str` / `join` buffers sized from real element lengths (not `n*24`).
- Huge array-index assignment guarded (`a[2e9]=1` no longer corrupts memory).
- Built with `-fwrapv` (signed-overflow defined) via a per-file Makefile rule.

## Files
- `kernel/js.c`, `kernel/include/js.h` — the interpreter.
- `kernel/syscall.c` (`SYS_js`), `user/shell.c` (`js` command), `tools/mkfatfs.c`
  (`SAMPLE.JS`).
