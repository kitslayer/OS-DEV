# Milestone 173 — nullish coalescing (`??`) and optional chaining (`?.`)

The two modern null-handling operators, plus an arena bump that the growing
engine needed.

## `??` — nullish coalescing

`a ?? b` yields `a` unless `a` is `null` or `undefined`, in which case it yields
`b`. Unlike `||`, it does **not** fall through on other falsy values — `0` and
`""` are kept:

```js
null ?? "default";       // "default"
undefined ?? 7;          // 7
0 ?? 99;                 // 0     (|| would give 99 — this is the point of ??)
"" ?? "x";               // ""
cfg.timeout ?? 30;       // 0 when timeout is 0, 30 when it's absent
```

Implemented as a logical operator (short-circuit code `'N'`, same precedence
level as `||`): evaluate the left side; return it unless it is `V_UNDEF`/`V_NULL`,
otherwise evaluate and return the right.

## `?.` — optional chaining

`a?.b` short-circuits to `undefined` when `a` is `null`/`undefined` instead of
faulting, and the three call/access forms are supported:

```js
user?.name                 // member
user?.addr?.city           // chains
arr?.[i]                   // index
obj.method?.()             // call (no throw if the method is absent)
fn?.()                     // call a maybe-null function
nobody?.a?.b?.c            // undefined, no crash, at any depth
nobody?.name ?? "anon"     // composes with ??
```

The lexer tokenises `?.` (unambiguous here — the kernel's Number is integer-only,
so there is no `?.5` decimal to collide with). In `parse_postfix` a `?.` produces
an optional `N_MEMBER`/`N_INDEX`/`N_CALL` (flagged via the spare `prefix` field).
At eval an optional member/index whose receiver is `null`/`undefined` returns
`undefined`; an optional call whose callee (or whose receiver, for
`obj?.method()`) is `null`/`undefined` returns `undefined` instead of raising
"not a function" / "no such method". Idiomatic per-link chains
(`a?.b?.c?.()`) short-circuit fully; a normal ternary `a ? b : c` is unaffected
(verified).

## Companion: arena 1 MB → 2 MB

`js_run` parses the **entire** program into the arena, then `install_globals`
and evaluation allocate on top of it — all in one bump allocator with no GC.
After this session's large OOP/ES6 additions, the kitchen-sink regression suite's
parsed AST plus the globals left too little headroom, so an `env_define` during
evaluation hit `g_oom` and a function binding silently became `undefined`
("not a function" several statements later — a capacity symptom, not a logic
bug). The arena is now 2 MB (static BSS, negligible on the kernel's RAM), giving
comfortable eval headroom above the AST for large scripts and pages. Verified:
the full suite runs (`make jstest`, ASan/UBSan-clean), the kernel builds clean,
and the OS boots and runs `js es6.js` correctly in-QEMU with the larger arena.
