# Milestone 129 — UTF-8 body text (the real web speaks UTF-8)

**Goal:** entities (milestone 128) only cover text written as `&name;`/`&#NN;`.
Most of the modern web instead emits **raw UTF-8 bytes** for smart quotes, dashes,
accented letters, and symbols. Our parser read the body byte-by-byte, so a UTF-8
"…" (`E2 80 A6`) became three garbage glyphs. This decodes UTF-8 in the text loop
and folds each codepoint to its ASCII lookalike (we have an ASCII-only 8×16 font).

## The change

A small `decode_utf8(s, maxlen, *cp)` reads one sequence (2/3/4-byte lead bytes
`0xC0–0xF7` + `0x80`-prefixed continuations), returns the bytes consumed, and on
any malformed/overlong/truncated input falls back to a single Latin-1 byte. The
`parse_html` per-character loop now branches: `&` → `decode_entity` (as before),
else a byte `>= 0x80` → `decode_utf8` then `uni_to_ascii`. Because milestone 128
already taught `uni_to_ascii` the Latin-1 accented letters and the common
typographic/symbol codepoints, both the entity path and the UTF-8 path share one
folding table.

## Verified

Added a `UTF8.HTM` disk fixture containing **raw UTF-8 bytes** (not entities) and
rendered it with `browse file:utf8.htm`:

- Smart quotes `“hello”`/`‘hi’` → `"hello"`/`'hi'`
- Em dash `—` → `-`, ellipsis `…` → `.`, bullet `•` → `*`
- Accents fold: `café`→`cafe`, `naïve`→`naive`, `jalapeño`→`jalapeno`,
  `Über`→`Uber`, `straße`→`strase`
- Symbols: euro `€`→`E`, degree `°`→`o`

No panics. The existing `ent.htm` entity fixture still decodes unchanged.

## Files
- `kernel/browser.c` — `decode_utf8` helper; `parse_html` text loop folds raw
  UTF-8 via `uni_to_ascii`
- `tools/mkfatfs.c` — `UTF8.HTM` fixture (raw UTF-8 bytes)
