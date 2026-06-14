# Milestone 91 — form `<input>` placeholders

**Goal:** make form controls visible. `<input>` elements rendered as nothing, so
a search box or login form was just blank space. Now each input shows a
placeholder, so you can see the form's fields and buttons.

![file:form.htm — a text field as "[Search the site]", a submit as "[ Go ]", a bare field as "[____]"](osdev-form.png)

## What it shows

`handle_tag` now renders `<input>` (reusing `find_attr` from milestone 89):

- has a `value` (a submit/button) → `[ value ]` (button-like),
- else has a `placeholder` → `[placeholder]`,
- otherwise → `[____]` (an empty text field).

All in the italic (EM) colour, inline where the field would be. `<button>` and
`<textarea>` already render their text content, so this fills the one gap —
self-closing `<input>` elements that previously left nothing on screen.

We can't *interact* with the fields (no in-page editing/submit yet), but the form
is now legible — you can see that a search box and a button are there.

## Verified (headless, by screenshot)

`file:form.htm` renders: *A search form:* `[Search the site]` `[ Go ]`, then
*And a bare field:* `[____]` *after.* — the placeholder text, the submit value,
and the empty-field marker all appear inline.

## Files
- `kernel/browser.c` — `<input>` handling in `handle_tag`
- `tools/mkfatfs.c` — `FORM.HTM` test fixture
