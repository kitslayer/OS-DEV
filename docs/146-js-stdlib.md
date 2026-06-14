# Milestone 146 — JavaScript standard library

**Goal:** make the from-scratch JS interpreter (milestones 144–145) genuinely
useful by adding the standard-library surface real scripts rely on — for both the
shell `js` and in-page `<script>`.

## What was added (all in `kernel/js.c`)

- **`Math`** — `abs`, `min`, `max`, `floor`/`ceil`/`round`/`trunc` (identity on the
  integer Number type), `sqrt` (integer Newton/bisection), `pow` (integer).
- **`JSON.stringify`** — recursive serializer for objects, arrays, strings (with
  escapes), numbers, booleans, null. Bounded 16 KB output, depth-guarded so a
  cyclic value can't recurse forever.
- **`Object.keys(o)`** → array of key strings.
- **Globals** — `parseInt`, `parseFloat`, `String`, `Number`, `Boolean`, `isNaN`.
- **String methods** — `includes`, `startsWith`, `endsWith`, `trim`, `repeat`,
  `replace` (first occurrence), `split` (incl. `""` → characters).
- **Array methods** — `slice`, `reverse`, and the higher-order `forEach`, `map`,
  `filter` (which call a JS function per element via the interpreter's
  `call_function`, so closures work as callbacks).

Plus a small `arr_push_val` helper and `def_native`/`obj_val` registration helpers.

## Verified

Host-tested under **ASan + UBSan** (`-DJS_HOSTTEST`), including adversarial cases:
- A self-referential value passed to `JSON.stringify` (`a.self=a`) — depth-guarded,
  no infinite recursion.
- `"x".repeat(2e9)` — rejected with a clean error (no integer-overflow / OOM).

Live in the OS, in one expression:

```
js -e print(JSON.stringify({os:"OS-DEV", sq:Math.sqrt(144), evens:[2,4,6].map(function(x){return x*x;})}))
{"os":"OS-DEV","sq":12,"evens":[4,16,36]}
```

## Files
- `kernel/js.c` — Math/JSON/Object/global natives, String/Array methods.
