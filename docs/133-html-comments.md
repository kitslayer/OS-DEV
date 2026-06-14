# Milestone 133 — parser robustness: HTML comments + inline SVG

Two related real-world-HTML fixes so the parser doesn't leak markup internals
into the rendered page.

## Part A — HTML comment skipping (real pages are full of them)

**Goal:** the tag scanner found the
end of a tag by scanning to the next `>`. But an HTML comment
`<!-- ... -->` can *contain* `>` — conditional comments (`<!--[if IE]>…<![endif]-->`),
embedded markup, or just prose with `=>`/`->`. The old scanner stopped at the
first inner `>`, so the rest of the comment (and any `<b>tags</b>` in it) leaked
into the rendered page as visible text. Every real page has comments, so this
was a pervasive low-grade corruption.

## The fix

When the parser sees `<` followed by `!--`, it scans to the proper `-->`
terminator (which may be many `>`s later) and skips the whole comment, rather
than treating it as an ordinary tag that ends at the first `>`. An unterminated
comment safely skips to end-of-input. `<!doctype …>` and other `<!…>`
declarations (no leading `--`) are unaffected and still skip as before.

## Verified

A fixture with two comments — `<!-- a comment with a > and <b>markup</b> inside …-->`
and a conditional `<!--[if IE]><p>conditional</p><![endif]-->` — placed between
the letters `A` and `B`. Rendered result: `A B` then "After comments." — **neither
comment's content (the inner-`>` tail, the `markup`, or `conditional`) rendered**,
and the surrounding text is intact. The existing entity/UTF-8/text fixtures are
unaffected. No panics.

## Part B — inline `<svg>` suppression

Modern pages embed inline SVG icons. SVG has its own `<title>`, `<text>`,
`<desc>` elements — so without special handling an SVG `<title>` would **hijack
the page/window title** and `<text>` content would render as stray words. Fixed
by treating `<svg>…</svg>` like `<script>`/`<style>`: an `insvg` flag suppresses
all content between the tags, and the `<title>`-capture branch is gated on
`!insvg` so an SVG title can't be mistaken for the document title.

Verified with an `<svg><title>svgtitle-must-not-show</title><text>SVGLEAK</text>
<path .../></svg>` in the fixture: neither `SVGLEAK` nor the SVG title rendered,
the window title stayed "Browser", and "After svg." rendered normally.

## Files
- `kernel/browser.c` — `parse_html` skips `<!-- … -->` to the `-->` terminator;
  `insvg` flag suppresses inline-SVG content and blocks SVG-`<title>` hijack
- `tools/mkfatfs.c` — `UTF8.HTM` fixture extended with comment + SVG test cases
