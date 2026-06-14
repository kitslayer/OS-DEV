# Milestone 154 — try / catch / finally / throw

Adds full exception handling to the JS interpreter (`kernel/js.c`).

- **`throw expr`** unwinds evaluation (sets the same `g_err` flag built-in errors
  use, plus `g_threw`/`g_throwval` to carry the thrown value).
- **`try { } catch (e) { } finally { }`** — the catch clears the in-flight error
  and binds `e` to the thrown value (for an explicit `throw`) or the error message
  string (for a built-in error). `finally` **always** runs, on a cleared error
  slate, then the pending exception/`return`/`break` is restored (a `return`/throw
  inside `finally` overrides it).
- **Built-in errors are catchable**: `try { JSON.parse("{bad") } catch(e){…}`,
  undefined variables, "not a function", array-index-out-of-range, etc. all become
  catchable exceptions. **`g_oom` (arena exhaustion) is deliberately NOT catchable**
  — catching resource exhaustion and continuing would loop.
- An uncaught `throw` ends the script with `[js error: <value>]`.

Host-tested under ASan+UBSan: throw strings/objects, catch built-in errors,
`finally` (normal / on-throw / with `return`), nested try + rethrow, and uncaught
throw. New keywords: `try catch finally throw`.
