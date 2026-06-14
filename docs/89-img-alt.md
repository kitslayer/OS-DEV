# Milestone 89 — `<img>` alt text

**Goal:** stop silently dropping images. The browser ignored `<img>` entirely, so
image-heavy pages had unexplained gaps. Now an image shows its **alt text** (or a
`[img]` placeholder), so you know something was there and what it was.

![file:img.htm — "[a cat photo]" and "[img]" shown inline where images would be](osdev-img-alt.png)

## What changed

- `find_href` was generalized into `find_attr(attrs, name, …)` — it now finds any
  named attribute, with a token-boundary check so a short name like `alt` can't
  match inside another attribute or value. `find_href` is a thin wrapper, and
  link parsing is unchanged (verified — see below).
- `handle_tag` handles `<img>`: it pulls the `alt` attribute and emits
  `[alt text]` as an italic-styled word, or `[img]` when there's no alt.

Images are inline (no line break), so the placeholder sits in the flow exactly
where the image would be.

## Verified (headless, by screenshot)

- `file:img.htm` renders: *A photo: `[a cat photo]` sits here, and a no-alt one:
  `[img]`. Text continues after the images.* — both the alt text and the
  no-alt placeholder appear inline in the distinct (EM) colour.
- **Regression check:** opened the start page, selected the first link with `n`
  and followed it with Enter → **example.com loaded**, confirming the
  `find_href`→`find_attr` refactor didn't break link `href` parsing.

## Files
- `kernel/browser.c` — `find_attr` (+ `find_href` wrapper), `<img>` handling in
  `handle_tag`
- `tools/mkfatfs.c` — `IMG.HTM` test fixture
