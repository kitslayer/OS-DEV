# Milestone 128 — HTML entities in attribute text + Latin-1 folding

**Goal:** a small but real-world-validated browser fidelity fix. Fetching
`https://www.gnu.org` over the new HTTPS client (milestone 127) rendered its logo
alt-text as the literal `[&nbsp;[A GNU head&nbsp;]` — the `&nbsp;` entities never
decoded. The body-text parser decodes entities, but the **`<img alt="...">`
attribute** is copied straight into the rendered label, bypassing that path.

## The fix

1. **`copy_decoded(dst, dstmax, src, srclen)`** — a helper that copies attribute
   text while running each `&entity;` through the existing `decode_entity`. The
   img-alt label now uses it, so `alt="&nbsp;[A GNU head&nbsp;]"` renders as
   `[ [A GNU head] ]`.
2. **Latin-1 folding in `uni_to_ascii`** — numeric refs like `&#233;` (é) used to
   fold to a blank space (the default), silently dropping accented letters from
   non-English pages. Now the Latin-1 Supplement letters (`À`–`ÿ`) fold to their
   base ASCII letter (é→e, ñ→n, ü→u, ß→s, …), so the text stays readable on our
   ASCII-only 8×16 font.
3. **More named symbol entities** — `&reg;`, `&trade;`, `&times;`, `&divide;`,
   `&deg;`, `&laquo;`/`&raquo;`, `&euro;` joined the existing table.

## Verified

- **gnu.org** (live HTTPS): the alt-text banners now read `[ [A GNU head] ]`,
  `[ Search www.gnu.org ]`, `[ Other languages ]` — spaces, not `&nbsp;`.
- **`file:ent.htm`** (the entity fixture): no regression — named (`'single'`,
  `"double"`, dash, ellipsis, bullet, copy), decimal, and hex entities all still
  decode correctly.

## Files
- `kernel/browser.c` — `copy_decoded` helper; `uni_to_ascii` Latin-1 folding +
  extra symbols; named-entity table additions; img-alt uses `copy_decoded`
