# M434 — Browser parser deep-audit (+ `decode_entity` hardening)

The earlier untrusted-input security audit ([docs/422](422-untrusted-input-security-audit.md))
swept the network → TLS → X.509 → crypto stack and the image decoders, found + fixed
two real **kernel** memory-safety bugs (a JPEG DRI out-of-bounds read, M422; a DNS
query-builder kernel-stack overflow, M423), and reviewed the browser's HTML tokenizer
(`parse_html`/`handle_tag`/`decode_entity` + the bounded `b->text` token pool) safe at
a **high level** — tokens are `(off,len)` slices into a bounded pool, copies are
dest-capped, entity refs clamped, the main loop always advances.

This pass goes **deeper**: a granular, bound-by-bound re-review of the browser's
*self-contained* sub-parsers — the CSS value/colour/selector parsers and the attribute
helpers — which the high-level pass mentioned only as "all tag/attr/entity copies are
dest-length-capped." It matters because the browser parses *arbitrary web HTML/CSS,
fetched over the network, in kernel context* — there are no guard pages on kernel
stacks, so an OOB read/write here is the same severity class as the JPEG/DNS bugs.

## Scope — the self-contained parsers in `kernel/browser.c`

These functions take raw byte slices (or NUL-terminated strings) and were reviewed
bound-by-bound, against their array sizes and **every** call site's contract:

| Function | Untrusted input | Binding safety bound |
|---|---|---|
| `decode_entity` | `&amp;`/`&#65;`/`&#x41;`/named entities in page text | reads bounded by `i < n < maxlen`; writes `*out` exactly once |
| `parse_color` | `#rgb`/`#rrggbb`, `rgb()/rgba()`, named colours | `d[6]`/`comp[3]`/`buf[16]` writes guarded `n<6`/`nc<3`/`p<15`; reads `i<vl`; ints capped |
| `style_prop`, `parse_style_color`, `parse_style_textstyle`, `parse_style_underline` | inline `style="…"` | every lookahead `s[i+OFF]` covered by `i+OFF < i+(OFF+1) ≤ n` (or `≤ ve ≤ n`); `s[i-1]` is `i>0`-guarded |
| `sel_parse` | CSS selectors (`<style>` rules + JS `querySelector`) | `sel_t` `tag[16]`/`cls,id,attr[32]`; writes capped `k<15`/`k<31`; every `s[i]` NUL-guarded; every branch advances `i` (terminates) |
| `find_attr`, `has_attr`, `attr_int`, `attr_eq`, `class_has` | tag attribute slices | `i+nl<=n` / `i+cl<=vl` scan bounds; `class_has`'s `i+cl==vl ||` short-circuit guards the `v[vl]` read |

**Result: all verified bounds-safe.** Every read is length- or NUL-bounded, every
array write is capped to its field size, and every loop terminates (each `sel_parse`
outer-loop branch advances the cursor; an unsupported selector char fails closed).
A second, independent subagent review reproduced the bound-by-bound proof and
returned CLEAN — the same review practice that caught the M422/M423 bugs.

## The one fix — `decode_entity` defense-in-depth

`decode_entity(s, maxlen, out)` reads `s[1]` (the `#` test for numeric entities). That
read is safe **because** both callers pass `s` pointing at a `'&'` (so finding the
closing `;` forces the scan to `n ≥ 1`, hence `maxlen ≥ 2`). But the safety was
*caller-enforced*, not self-contained: a hypothetical future caller passing `s[0]==';'`
with `maxlen==1` would read `s[1]` one byte out of bounds.

Added a single guard so the function is safe regardless of caller:

```c
static int decode_entity(const char *s, int maxlen, char *out) {
    if (maxlen < 2) return 0;   /* shortest decodable entity is >=2 chars; also makes
                                   the s[1] read below in-bounds regardless of caller */
    ...
```

This has **zero behavioural change** for real inputs: every reachable call already
returned 0 when `maxlen < 2` (the shortest decodable entity is `&…;`), so the guard
only closes the latent hole — it never alters a real decode.

## Verification

- Clean rebuild (`make`) + clean boot (no `#PF`/panic in the serial log).
- `browse file:ENT.HTM` renders the dedicated entity page correctly end-to-end —
  named (`'single'`/`"double"`, em-dash, bullet `*`, copy `c`), symbol-folds
  (arrows `< ^ > v`, `±→+`, `×→x`, `÷→/`, `°→o`, `§→S`, `¶→P`), numeric **decimal**
  (`&#39;`→`'`, `&#8220;`/`&#8221;`→`"`), and numeric **hex** (`&#x27;`→`'`,
  `&#x2014;`→em-dash) — exercising the touched code path with the guard in place.

With this pass, every kernel-side untrusted-input parser the OS exposes — network,
TLS, X.509, crypto, image decoders, and the HTML/CSS browser (now down to the
**individual sub-parser** level) — has been audited bounds-safe (two real bugs found
and fixed along the way: the JPEG DRI OOB-read and the DNS-query overflow).
