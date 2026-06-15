# 281–283: `querySelector` / `querySelectorAll` — CSS selectors over a tree-less DOM

Before this arc, the browser's "DOM" could only address an element **by id**:
`document.getElementById("x")` and `querySelector("#x")` returned a handle whose
sole identity was the id string (stored in `vals[0]`), and every read/write
(`.textContent`, `.innerHTML`, `.value`, `getAttribute`, `setAttribute`,
`remove`) located the element by scanning the page source for `id="x"`.

That left out the single most common DOM query in real JavaScript: finding
elements by **tag** or **class** — `querySelectorAll(".item")`,
`getElementsByTagName("p")`. This arc adds them.

## The blocker: an id-less match has no name to address it by

The renderer here is a **flat token stream, not a DOM tree** (see
[docs/192](192-dom.md)-ish history): the "DOM" works by mutating the page
*source* (`b->raw`) and re-rendering. So the handle returned by a query has to
carry enough to *re-find* its element later — and a `.item` match usually has
no id. The whole reason `querySelectorAll` had been deferred was: **how do you
address a matched element that has no id?**

## The solution: a byte-offset position handle (additive)

A `V_ELEMENT` handle may now carry a second slot:

- `vals[0]` = the id string (legacy id handles) **or** `""` (a query match).
- `vals[1]` = an **integer byte offset** of the matched element's opening `<`
  in `b->raw` (a query match only; absent on id handles).

`element_handle_at(off)` builds the position variant; `element_handle(id)` is
**unchanged**, so an id handle still has `n==1` and never grows a `vals[1]`.
Every consumer checks `has_pos = (el->n > 1 && el->vals[1].t == V_NUM)` and
routes to a **parallel** set of host callbacks (`*_at`) that resolve the offset
to the same byte span the id-keyed splice code already uses. **The id path is
byte-identical** — the new path is reached only when a page calls
`querySelectorAll`/`getElementsBy*`, or `querySelector` with a non-`#id`
selector. (Pure `#id` keeps the fast id handle.)

```
  document.querySelectorAll(".fruit")
        │  nat_querySelectorAll  (js.c)
        ▼
  g_dom_query(".fruit", offs[], max)        ── browser.c: browser_dom_query
        │  sel_parse + sel_match_all (scan b->raw for <tag class="…fruit…">)
        ▼  offs = [o1, o2, o3]   (byte offsets of each matching '<')
  [ element_handle_at(o1), element_handle_at(o2), element_handle_at(o3) ]
        │  a plain V_ARR — so .length / [i] / forEach / for-of all work for FREE
        ▼
  el.textContent              el.textContent = "x"          el.getAttribute("c")
   g_dom_get_at(off,…)         g_dom_set_at(off,…)           g_dom_getattr_at(off,…)
   dom_find_at(off)→[is,ie)    splice b->raw, re-render      dom_attr_region_at(off)
```

## The selector matcher (`sel_match_all`)

A tiny CSS matcher supporting `tag`, `.class`, `#id`, and compounds
(`p.fruit`, `div#main`). It scans `b->raw` for opening tags (reusing the proven
`parse_html` quote-aware tag scan) and, for each, checks the tag name
(`tageq`), the `class="…"` value with word-boundary matching (`class_has`), and
`id` (`attr_eq`) — all via the existing attribute helpers `find_attr` /
`tageq` / `attr_eq`. The scan is bounded to the body region, the result count
is capped (`QSA_MAX` = 256), and there is no recursion or allocation — safe on
the guard-page-less 256 KB kernel stack.

## The `*_at` bridge: duplicate, don't refactor

`dom_find_at` / `dom_attr_region_at` are position variants of `dom_find` /
`dom_attr_region` — the *"scan for `id=` then back up to `<`"* preamble is
replaced by *"validate there's a real opening `<` at `off`"*; the rest (the
depth-counted close-tag search, the attr-span find) is identical. The write
functions `browser_dom_set_at` / `browser_dom_setattr_at` **duplicate** the
splice body of `browser_dom_set` / `browser_dom_setattr` rather than refactoring
a shared core. That is deliberate: the browser is a working north-star system,
and duplication means the id-keyed splice executes the exact same bytes as
before — **zero regression surface** on the existing
`getElementById`/`document.write` interactivity, at the cost of ~50 duplicated
lines.

## Memory safety on untrusted input

Both the page HTML and the page JS are untrusted, and the kernel stack has no
guard page, so the splice must be bullet-proof:

- `dom_find_at` / `dom_attr_region_at` **re-validate** `off` (`off ≥ bodyoff`,
  `off+1 < bodyend`, `r[off]=='<'`, `r[off+1]` alphanumeric) before use. A
  forged, negative, huge, or **stale** offset therefore fails closed (no-op) or
  resolves to a still-valid `<` — it can never index outside the buffer. Even
  if a page forges `vals[1]` to an arbitrary number, the `(int)`-cast result is
  still bounds-checked here.
- The splice keeps `browser_dom_set`'s `live_end + delta` bound against
  `RAW_MAX`, and the document.write cursor sync (`g_sw_pos`/`g_sw_base`) for the
  load-time-script case.

**Staleness (a correctness, not safety, caveat):** after one position write,
the page source shifts and re-renders, so other position handles into the same
page hold stale offsets. A single-match write (`querySelector("p.x").textContent
= …`) is exact; a `querySelectorAll(...).forEach(el => el.textContent = …)`
multi-write is **best-effort** after the first reflow — it may edit the wrong
element or no-op, but (per the validation above) never goes out of bounds.

## A footnote: the arena had to grow

The single-run, no-GC regression suite (`tests/js/suite.js`) accumulates every
allocation until the run ends. Adding the querySelector tests pushed it just
past the **8 MB** arena, surfacing as a spurious `"no such method"` mid-run
(an allocation failing under pressure, not a feature bug — confirmed by running
the *same* suite at 16 MB, where it passes cleanly). The arena is static BSS the
code already calls "cheap on the kernel's RAM", so it was bumped to **12 MB**
for headroom. OOM is graceful (`aalloc → g_oom → NULL`), so this is a capacity
knob, not a safety one.

## Verified

- `make jstest` — host ASan/UBSan, golden-locked (querySelector,
  querySelectorAll, getElementsByTagName/ClassName, getAttribute, and write
  read-back all asserted against a mock DOM).
- In-OS (`file:qsa.htm`): `count=3`, `first p.fruit=Apple`, missing→`null`,
  `byTag p=4`, `byClass fruit=3`, `first fruit class=fruit`; renders
  "First match: Apple" / "Found 3 fruit: Apple Banana Cherry".
- In-OS (`file:qsaw.htm`): a load-time script rewrites the first `p.msg` via a
  position handle — the page re-renders showing the new text (2nd paragraph
  untouched) and `setAttribute`+`getAttribute` round-trip `data-done=yes`.

## Follow-ups (M284–286)

- **M284 `[attr]` selector.** `sel_parse` gained a `[name]` clause (any `=value`
  is ignored — presence-only) and `sel_match_all` one `has_attr` check (which
  matches both valued and boolean attributes). Additive to the matcher; the
  existing tag/class/id branches are untouched, so the M283 review's safety
  analysis of them still holds. `querySelectorAll("[class]")` / `"p[class]"` work.
- **M285 `element.classList`** (`add`/`remove`/`toggle`/`contains`). The single
  most-used DOM API, and it needed **no new browser code**: `el.classList`
  returns a `V_CLASSLIST` handle that carries the element's id/offset addressing,
  and its methods read & rewrite the class attribute through the *existing*
  `get`/`setAttribute("class")` callbacks — so it works for both id handles and
  querySelector position handles. All token work is bounded static-buffer string
  manipulation (no raw page-buffer splice is added). Because a position handle's
  own opening `<` doesn't move when you edit *inside* its element, chaining
  `el.textContent = …; el.setAttribute(…); el.classList.add(…)` on the *same*
  match all work in one script (the staleness caveat only bites *other* matches).
- **M286 `element.hasAttribute`.** Completes get/set/has on the attribute API;
  reuses the getAttribute callbacks (no new code), works for id + position handles.

So the selector grammar (tag, `.class`, `#id`, `[attr]`, compounds) and the
read/write/attr/classList surface are complete on the token-stream renderer.
Only node *construction* (`createElement`/`appendChild`) and event listeners
(`addEventListener` — needs a persistent per-page JS env, since the arena resets
per run) remain — both genuinely need machinery the token-stream model lacks.
