# Milestone 199 — HTML form submission (GET)

Editable fields (M198) let you *type* into a form; this milestone lets you
**submit** one. A submit button now gathers the form's named fields into a
`action?name=value&…` query string, URL-encodes it, and navigates — a real
HTML GET form submission. Point the `action` at a search engine and the same
form is a working web search box.

Verified end-to-end in the real kernel over **real HTTPS** as an actual **web
search** (`SEARCH.HTM` points its `action` at DuckDuckGo's HTML endpoint): typed
`from scratch os` into the field, followed `[Search DuckDuckGo]`, and the browser
navigated to

    https://html.duckduckgo.com/html/?q=from+scratch+os

(36590 B, `TLS*` = chain anchored to DigiCert Global Root G2) and rendered real
results — fittingly, *"Guide to Build an Operating System From Scratch"* with its
snippet and link. Both spaces became `+`. The same form against `example.com`
shows the query string land in the address bar (`?q=…&cat=general`, including a
seeded default field). So the OS can now **search the live web** from a typed
query — type → build `?q=…` → HTTPS fetch + cert-validate → render results.

## How to submit a form

1. Tab/`n` to a field, Enter to focus it, type, Enter to finish (M198).
2. Tab/`n` to the `[Submit …]` button and press Enter.
3. The browser builds the query and navigates; the address bar shows the full
   URL with the query string.

## How it works

It reuses the existing field store and link/click machinery — no new subsystem:

- **`<form action="…">`** — `handle_tag` captures the action URL into
  `b->form_action` (reset at the top of every parse, on `</form>`, and on
  navigation). Relative actions (`/search`, `search`) resolve against the
  current page via the existing `goto_href`; an empty action submits to the
  current page (its URL up to any `?`).
- **Named fields** — alongside the id-keyed value store (`in_id`/`in_val`), each
  field's `name=` attribute is recorded in a parallel `in_name[]`. A field with
  a `value=` attribute is seeded into the store at render time, so its default
  is both shown and submittable even if never focused. `type="hidden"` fields
  are seeded but not drawn.
- **Submit button** — `<input type="submit">` (or `type="image"`) renders
  `[ value ]` as a link whose href is `submit:` + a **snapshot** of the form's
  action (snapshotted because the parse-time `form_action` is cleared by the
  `</form>` close tag, long before the click).
- **Submission** — following a `submit:` link routes (like `input:`/
  `javascript:`) into `browser_follow`, which builds the query: the action,
  then `?`/`&` + `url_encode(name)` + `=` + `url_encode(value)` for each stored
  field with a non-empty name, then `goto_href`. `url_encode` passes unreserved
  characters through, maps space to `+`, and percent-encodes the rest.

Every buffer write is bounded for the kernel's guard-page-less stack: the query
builds into a fixed `q[URL_MAX]` (160 B) with each append guarded, and
`url_encode` reserves room for a worst-case `%XX`. Reviewed clean (ASan/UBSan
fuzz of `url_encode` + the query-build loop).

## Limitations / next

- **GET only** — POST would need a request body in the fetch path.
- **One form per page** for field scoping: a submit gathers *all* stored named
  fields (fine for the common single-form/search-box page; multi-form pages
  send extra params, which servers ignore).
- The store holds 8 fields × 95 chars; the whole URL is capped at 160 bytes.
- Submitting on **Enter inside a field** isn't wired (use the submit button);
  `<button type="submit">` (vs `<input type="submit">`) needs an `onclick` for
  now.

The change is additive — the new `submit:` scheme and the `type=submit`/`hidden`
branches don't alter the verified text-field render or typing paths. `make
jstest` clean (js.c untouched); kernel builds clean; verified interactive in-OS
over HTTPS.
