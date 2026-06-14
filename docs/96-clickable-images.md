# Milestone 96 — clickable inline images

**Goal:** let you actually *view* the images referenced inside a page. Milestone
89 showed `<img>` as `[alt text]`; now that text is a **link to the image**, so
you can follow it (keyboard or click) and the browser renders the picture.

![file:img.htm — the two `<img>` elements rendered as links "[the test image]" and "[an icon]"](osdev-img-links.png)

## How it works

This deliberately reuses the existing link + image machinery instead of building
async multi-image fetching:

- In `handle_tag`, an `<img>` with a `src` registers that URL as a link
  (`add_href`) and emits its `[alt]` (or `[img]`) text as a `STY_LINK` word — so
  it's selectable with Tab/`n` and followable with Enter, exactly like an `<a>`.
- Following it resolves the `src` and navigates there; if it's a PNG, the image
  decoder (milestones 92–95) renders it. `goto_href` now treats **`file:`** as an
  absolute scheme (alongside `http://`), so `<img src="file:…">` loads directly.

So a page's images become a list of links you can open one at a time — the
practical bridge to inline images without the complexity of fetching and laying
out N images concurrently (which the single fetch worker / response buffer would
make fiddly; noted for future work).

## Verified (headless, by screenshot)

`browse file:img.htm` (a fixture whose two `<img>` tags point at `file:test.png`
and `file:icon.png`) renders **"[the test image]"** and **"[an icon]"** as blue
links. Selecting the first with `n` and pressing Enter navigates to
`file:test.png` and **renders the image** — confirming the whole chain
(img→link→follow→`file:` resolution→decode→blit). No panics.

## Files
- `kernel/browser.c` — `<img>` emits a link to its `src`; `goto_href` treats
  `file:` as an absolute scheme
- `tools/mkfatfs.c` — `IMG.HTM` now references real on-disk images
