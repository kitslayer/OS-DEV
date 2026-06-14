# Milestone 151 — clickable JavaScript (`javascript:` links)

**Goal:** make the browser **interactive** — clicking a link can run JavaScript
that modifies the page, not just navigate. The first real step beyond
load-time `document.write` (milestone 145).

## What

- A link whose href is `javascript:CODE` (`<a href="javascript:...">`) now **runs
  CODE on click** instead of navigating. `document.write` output is spliced into
  the page and it re-renders in place; `console.log` goes to the serial log.
- Reuses the existing link machinery (selection, click rects, `goto_href`): the
  only new branch is at the top of `goto_href` — `javascript:` → `run_js_handler`,
  everything else navigates as before.
- `run_js_handler` appends `document.write` output after the page body (the body
  region `bodyoff/bodylen` is now remembered per page) and re-parses, so repeated
  clicks keep appending.

## Also: quote-aware attribute scanning

The HTML tag scanner now tracks quotes when scanning to the closing `>`, so a `>`
inside a quoted attribute (e.g. `href="javascript:document.write('<p>...')"`, or
`alt="a > b"`) no longer truncates the tag. Strictly more correct; behaviour is
identical for normal tags (their `>` is unquoted).

## Verified

`JSCLICK.HTM` (baked on the disk) has three `javascript:` links. Clicking
**“compute 6 \* 7”** runs `document.write('<p><b>6 * 7 = '+(6*7)+'</b></p>')` and
**“6 \* 7 = 42”** appears, bold, on the page — live in the OS. Normal links
(http/https/file/relative) and Back are unaffected.

## Files
- `kernel/browser.c` — `run_js_handler`, the `goto_href` `javascript:` branch,
  `bodyoff/bodylen` tracking, quote-aware attribute scan.
- `tools/mkfatfs.c` — `JSCLICK.HTM`.
