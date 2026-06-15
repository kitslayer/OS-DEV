# 287: A persistent per-page JavaScript environment

For a long time the browser's page JS had a hard ceiling: **a function or
variable defined in a page's load `<script>` was gone by the time a click ran.**
`document.getElementById('b').onclick = function(){…}` couldn't work, and
neither could `addEventListener`, because the handler function — defined at load
— simply didn't exist anymore when the click happened.

## Why: the arena reset per run

The engine has no garbage collector. It uses one big bump arena
(`g_arena_buf`), and `js_run_impl` **reset it to empty (`g_arena_off = 0`) at the
start of every run** and built a fresh global environment (`install_globals`).
That's correct and necessary for the shell's `js` command (each invocation is
independent), and it's how page load scripts and click handlers both ran: each
through its own `js_run_doc` → `js_run_impl`. So a click handler started with a
blank slate — the load script's `function greet(){…}` and `var clicks` were in
an arena region that had been wiped and an env that no longer existed.

## The fix: a mode, and one persistent env

`js_run_impl` now takes a `mode`, and there's a single static `env *g_page_env`:

| mode | entry point | arena | env |
|------|-------------|-------|-----|
| 0 | `js_run` / `js_run_doc` (shell, document.write) | reset | fresh, not saved |
| 1 | `js_page_load` (a page's load `<script>`) | reset | new, **saved to `g_page_env`** |
| 2 | `js_page_event` (onclick / onchange / `javascript:`) | **kept** | **reuse `g_page_env`** |

So when a page loads, its global env is built once and **persisted**; every later
event handler runs in that same env (no arena reset), seeing the load script's
functions and variables — and its own writes persist too (a counter incremented
in an onclick survives to the next click, with no `localStorage`).

`js_page_reset()` (sets `g_page_env = 0`) is called in `browser_navigate`'s
per-page prologue, so a **new page never inherits the previous page's globals**.
A script-less page is safe: `g_page_env` is 0, so its first event rebuilds the
env from scratch (and resets the arena at that point).

```
  navigate ──► browser_navigate(): js_page_reset()  (g_page_env = 0)
                 │
                 ├─ load <script> present?  ──► js_page_load  (mode 1: reset, new env, persist)
                 │
   click ───────► run_js_handler ──► js_page_event (mode 2: KEEP arena, REUSE env)
                                       └─ sees greet(), clicks, … from load
```

`browser.c` wires it: `run_page_scripts` → `js_page_load`, `run_js_handler` →
`js_page_event`. **The shell `js` path is byte-unchanged** (mode 0): it still
resets per run and never touches `g_page_env`.

## Why it's memory-safe (the subtle part)

- **No dangling env.** `g_page_env` points into the arena. It is only *reused* in
  mode 2 (events), and it is set to 0 on every navigation *before* any event of
  the new page can fire. The next `js_page_load` resets the arena and installs a
  new env, overwriting the pointer. There is no path where a mode-2 event reads
  an env from an arena region that was reset out from under it.
- **The load AST stays valid.** A function defined at load is an AST node living
  in the arena. Because mode-2 events do **not** reset the arena, that node
  remains valid across events. Crucially, the AST holds **no pointers into
  `b->scripts` / `b->raw`** — every identifier and string literal is
  `intern()`-copied into the arena at parse time (a load-bearing invariant from
  earlier reviews). So even though a click triggers a DOM re-render that rewrites
  `b->scripts`, the persisted function's AST (in the arena) does not dangle.
- **Bounded growth.** Mode-2 events never reclaim memory, so each click's AST and
  temporaries accumulate in the arena until the next navigation. That's bounded
  by the 12 MB arena; exhaustion is graceful (`aalloc` → `g_oom` → a clean error,
  never an OOB write). A pathological click-spamming page degrades (stops
  updating) rather than corrupting.
- **Serialized.** `js_busy` still guards against re-entrant runs.

## What it unblocks

This is the prerequisite for **`el.onclick = fn`** and **`addEventListener`** —
storing a handler *function* (a closure in the persistent env) and firing it on a
later event. That's the next increment; M287 is the foundation: persistent
globals across the load script and all event handlers, verified in-OS
(`file:persist.htm`: a load-defined `greet()` and a `clicks` counter that
survives across clicks → "Hello, Ada! (clicks: 2)").

## M288: that next increment — `el.onclick = fn` / `addEventListener`

With the persistent env in place, JS-assigned handlers followed directly:

- **Registry.** `el.onXXX = fn` (assign hook) and `el.addEventListener(type, fn)`
  store the function in a per-page `@handlers` object (keyed `"type:id"`) bound in
  `g_page_env` — so it lives and dies with the persistent env (survives clicks,
  dropped on navigation), no extra lifetime plumbing.
- **Making the element clickable after render.** The handler is assigned at
  *runtime*, after the page was rendered, so the element isn't a link yet.
  `register_handler` writes a synthetic **`data-jsh`** attribute (through the
  existing `setAttribute` → re-render path); the renderer's `handle_tag` then turns
  any `data-jsh`+`id` element into an **`event:ID`** link (checked after inline
  `onclick`, before the button-default-submit — so existing pages render
  byte-identically). Clicking it routes through `browser_follow` → `run_js_event`
  → **`js_fire_event(id,"click")`**, which looks up the fn and invokes it via
  `call_function_this` in the persistent env (no arena reset).
- **v1 keys by id.** A position offset would be stale the instant the
  `data-jsh` write re-renders and shifts the buffer; an `id` is re-resolved each
  fire. Id-less elements are a no-op for now.

A subtle bug surfaced in the in-OS test and is worth remembering: **`obj_set`
stores the key *pointer*, not a copy** (fine for the string literals it's usually
given). The registry key is built in a stack buffer, so it must be `intern()`'d
into the arena before `obj_set`, or it dangles and the handler is never found.

Verified in-OS (`file:events.htm`): a load script does
`document.getElementById('go').onclick = function(){…}` and
`getElementById('add').addEventListener('click', …)` with **no** inline
`onclick=`; clicking fires the stored functions (onclick state persists across
clicks; addEventListener updates the DOM), and the inline-`onclick` path
(`dom.htm`) is unregressed.
