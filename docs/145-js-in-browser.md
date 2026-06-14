# Milestone 145 — JavaScript runs in the browser

**Goal:** wire the from-scratch JS interpreter (milestone 144) into the web
browser so pages can **run their `<script>` tags** — the foundation every
interactive web page is built on.

## What

The browser is a token-stream renderer (HTML → flat word tokens, no DOM tree),
so this is the pragmatic first increment that fits that model: **inline script
execution with `document.write()`**.

1. **Capture** — `parse_html()` now collects the text inside `<script>...</script>`
   into `b->scripts` (it already skipped script *rendering*; now it keeps the
   source). Multiple scripts are concatenated.
2. **Run** — after the page parses, `run_page_scripts()` executes the collected
   source via **`js_run_doc()`**, a JS entry point that routes `document.write(s)`
   to a host callback instead of the print buffer.
3. **Splice + re-render** — `document.write` output is appended into the page's
   HTML buffer (`b->raw`, after the body) and the page is **re-parsed once**, so
   script-generated markup renders as real HTML (headings, lists, bold, links…).
4. `console.log()` from a page goes to the serial log (`[js] …`).

So a page like:

```html
<h1>JavaScript in the browser</h1>
<script>
for (var i = 1; i <= 5; i++) document.write("<p>Item " + i + ": " + (i*i) + "</p>");
var s = 0; for (var k = 1; k <= 100; k++) s += k;
document.write("<h2>Sum of 1..100 = " + s + "</h2>");
</script>
```

renders the heading, the five generated `<p>` items, and a `<h2>` showing `5050`
— all produced by JavaScript executing inside the page. Baked on the disk as
`JSTEST.HTM` (`browse file:jstest.htm`).

## Safety

- **Additive**: a page with no `<script>` renders exactly as before (verified) —
  the only change on that path is a `b->scriptlen = 0` reset.
- **Bounded**: script capture is capped at `SCRIPT_MAX` (16 KB); `document.write`
  output is bounded by the 128 KB `RAW_MAX` buffer; the re-parse runs **once**
  (no script re-execution / infinite loop).
- **Serialized**: `js_run` now has an irq-guarded `js_busy` flag, because the
  browser (WM thread) and the shell's `js` (a ring-3 syscall) are distinct
  preemptible tasks that share the interpreter's static arena — only one runs at
  a time (the same pattern as `tls_get`).

## Honest limits (what this is *not*)

This is `document.write`-level scripting, not a DOM. `getElementById` returns
`undefined` (operations on it no-op safely), there are no event handlers yet, and
no CSS. So it runs page scripts and renders their generated HTML — it does **not**
make JS-driven single-page apps (Google Docs, etc.) interactive; that needs a real
DOM + layout + event loop, which is future work. `document.write` content is
appended after the body (not spliced at the script's exact position).

## Files
- `kernel/browser.c` — `capture_script()`, `run_page_scripts()`, the parse hook.
- `kernel/js.c`, `kernel/include/js.h` — `document.write`, `js_run_doc()`, the
  `js_busy` guard.
- `tools/mkfatfs.c` — `JSTEST.HTM`.
