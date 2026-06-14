# Milestone 198 — editable form fields (user text input)

The browser could already *display* `<input>` fields and run JS that mutates the
page, but you couldn't **type** into a field — input was the missing half of
interactivity. Now fields are editable, and JS reads them via `.value`, closing
the loop: **user input → JS processing → DOM output**.

Verified end-to-end in the real kernel (`FORM.HTM`): typed "Ada" and "21" into
two fields, clicked a button whose `onclick` reads both values, and it rendered
*"Hello Ada! Your number doubled is 42"* — `getElementById('num').value` parsed
and doubled.

## How to use a field

1. Tab/`n` to a field (`<input id="x">`), Enter to **focus** it.
2. Type — characters appear in the field; Backspace deletes; Enter/Esc finishes.
3. JS reads/writes it: `document.getElementById('x').value`.

## How it works

It reuses the existing patterns rather than adding new subsystems:

- **Storage** — each field's text lives in a small per-page store keyed by id
  (`b->in_id[]`/`in_val[]`), reset on navigation like `localStorage`.
- **Rendering** — `<input id="x">` renders `[ value ]` from the store (or the
  `value`/`placeholder` attribute, or `[____]`), with a `|` cursor when focused.
  A field with an id is emitted as a **selectable link** (`add_input_link` stores
  an `input:x` href), so it joins the Tab/Enter focus order.
- **Focus** — following an `input:x` link (Enter) sets `b->focus_id = "x"` and
  re-renders (mirrors how `javascript:` links route to `run_js_handler`).
- **Typing** — `browser_key`, when a field is focused, captures printable keys
  into the store and re-renders — the same approach as the in-page-find (`\`) and
  URL-edit modes. When no field is focused (`focus_id` empty) the handler is
  skipped, so normal navigation is unchanged.
- **`.value`** — a new `kind=2` in the DOM bridge: `element.value` read/write
  goes through the same `js_set_dom` callbacks as `textContent`/`innerHTML`, but
  maps to the field store (`in_get`/`in_set`) rather than the page source.

## Limitations / next

`type="password"` masking, `<textarea>`, checkboxes/radios, real form
*submission* (GET → query string), and an `onchange`/`oninput` event aren't
implemented — `getElementById(id).value` + a button's `onclick` covers the
input→process→output loop. The store holds 8 fields × 95 chars.

The change is additive (the new input-focus key mode only activates when a field
is focused; non-`<input>` rendering and normal navigation are untouched). Reused
the click path + find-mode patterns. `make jstest` clean; kernel builds clean;
verified interactive in-OS.
