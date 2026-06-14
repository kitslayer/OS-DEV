# Milestones 205–215 — DOM attributes, `window.location`, form controls & events

After form submission and web search (M199–M204, see
[docs/199](199-form-submit.md)), this arc made the in-page DOM read/write and
the forms genuinely *reactive* — a from-scratch take on the interactive web
platform. Every milestone was verified in the running kernel and the
substantial ones passed an ASan/UBSan security review (the browser parses
untrusted HTML and runs untrusted page JavaScript on a guard-page-less kernel
stack, so memory safety is reviewed hard).

## The DOM bridge, recapped

The renderer is a flat **token stream**, not a DOM tree. So "the DOM" operates
on the page *source* (`b->raw`) and re-renders: js.c intercepts `V_ELEMENT`
member access/method calls and calls host callbacks (`js_set_dom`,
`js_set_dom_attr`, `js_set_location`) that browser.c backs by locating the
element in `b->raw` and splicing. The load-bearing safety invariant: the JS AST
holds **no live pointers into `b->raw`/`b->scripts`** (everything is interned
into the arena), so a mutation that triggers `parse_html` mid-script can't
dangle the running interpreter.

## What landed

- **M205 `getAttribute(name)`** — reads any attribute off an element's opening
  tag. `dom_attr_region` locates the tag's attribute span (mirroring
  `dom_find`), then the existing `find_attr` returns the value; missing → `null`.
  Added a `V_ELEMENT` method-call path (`eval_element_method`, wired beside
  Map/Set/Date in the call dispatch) — the template for all element methods.
- **M206 `setAttribute(name, value)`** — the write counterpart. Replaces the
  attribute's value span (or inserts ` name="value"` before `>`) and re-renders,
  splicing `b->raw` exactly like the reviewed `textContent` path. The value has
  quotes stripped so it can't break out of the attribute. Visible effect: JS can
  swap an `<img src>`, retarget an `<a href>`, change `data-*`.
- **M207 `window.location`** — a read-only snapshot of the current URL
  (`href`/`protocol`/`host`/`pathname`/`search`), built at `install_globals`
  from a URL the browser sets via `js_set_location(b->url)` before running page
  JS. So a results page's script can read its own `?q=…`.
- **M208 address-bar replace-on-type** — the first keystroke after focusing the
  address bar clears it, so typing a search/URL needs no backspacing.
- **M210 checkboxes & radios** — `<input type=checkbox|radio>` render as
  `[x]`/`(o)` toggle links (`check:` scheme; Enter flips the stored `on`/``).
  Initial state from a valueless `checked` attribute (a new `has_attr` boolean-
  attribute scanner). A checked box submits `name=on`; an unchecked one is
  omitted (the render sets the submit name only when checked). `.value` reads
  `on`/`` via the existing bridge.
- **M211 radio group exclusion** — selecting a radio clears the stored value of
  same-`name` siblings, so groups are properly mutually exclusive.
- **M212 / M213 / M215 events** — `onchange` (checkbox/radio toggle, and text
  inputs on blur) and `oninput` (text inputs, every keystroke — search-as-you-
  type / live validation). All route through one helper, `fire_handler(b, id,
  attr)`: it reads the inline `on*` attribute, copies it off `b->raw`, and runs
  it via the reviewed `run_js_handler`. Page JS has no primitive to synthesize a
  click/toggle/keystroke and `js_busy` blocks nested runs, so handlers can't
  recurse.
- **M214 `Array.reduceRight`** — completes the reduce family (engine side).

## The result

Forms are now complete (`text`/`password`/`submit`/`button`/`hidden`/
`checkbox`/`radio`) and reactive (`onclick`/`onchange`/`oninput` + submit +
Enter-to-submit), and page JS can read & write element attributes and inspect
the URL. Combined with M199's web search, the browser is a genuinely
interactive web application built entirely from scratch.

## Not done (need a fresh focused effort or guidance)

`addEventListener`/`el.onclick = fn` (needs a persistent per-page JS env — the
arena resets per run); `querySelectorAll`/`getElementsByTagName` (the DOM
handle is keyed by id, so id-less matches can't be addressed without
position-based handles); `createElement`/`appendChild` (no DOM tree);
`<textarea>` multiline (the token renderer would show a single-line preview);
CSS/layout; robust out-of-order TCP for large CDN pages.
