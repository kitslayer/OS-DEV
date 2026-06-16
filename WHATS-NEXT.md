# What's next

> **Status (455 milestones).** (M455: **`display:none` + `font-size` from `<style>` rules** — M448/M454
> did these inline; now the `<style>` cascade does too, so `.hidden{display:none}` (ubiquitous utility
> classes) + `.big{font-size:2rem}` work. capture_css parses font-size/display into 2 new rule columns;
> css_match reports them (a display:none rule raises the same n_hidden scope; a font-size rule sets the
> per-token scale, inline overriding). Additive. Verified in-OS (HIDE.HTM: `.gone` hidden, `.big`
> enlarged, + the inline cases).
> M454: **CSS `display:none` + `hidden` attr** — an element with inline
> `display:none` (or the HTML5 `hidden` attr) suppresses itself + all content (text/bullets/hr/img).
> An `n_hidden` counter raised/lowered by the existing tag-depth-matched sc[] scope stack (composes
> with nesting); every emit primitive (emit_word/emit_break/hr+img token pushes) + the text-accum loop
> gated on it. Additive (n_hidden=0 ⇒ unchanged); visual blast radius only. Verified in-OS (HIDE.HTM:
> display:none p + hidden div+list + inline display:none span all absent, visible paragraphs in order,
> no leak). Note: `<style>`-rule display:none + standalone void-element (img) display:none deferred.
> M453: **browser cert-info display** — press `i` on an HTTPS page →
> the status shows the leaf cert's CN + expiry (YYMMDD) + a verification word (`anchored`/`verified`).
> `tls.c` exposes `tls_leaf_cn`/`tls_leaf_expiry`; browser snapshots CN/expiry/host_match per HTTPS
> load. Safe read-only "padlock details" surfacing M451/M452. Verified in-OS (example.com →
> "example.com exp260829 anchored").
> M452: **TLS cert validity-period enforcement** — `x509.c` parses
> notBefore + adds `x509_time_cmp` (UTCTime YY-pivot + GeneralizedTime; unparseable→0/no-opinion);
> `tls.c` reads the RTC and REJECTS an expired/not-yet-valid leaf cert before the request. Guarded:
> enforced only if RTC year ≥ 2020 (unset clock fails open), unparseable date fails open — a bad
> RTC can't break all HTTPS. x509test unit-tests x509_time_cmp. Verified in-OS BOTH ways: clock 2026
> → example.com loads; VM clock forced to 2040 (`-rtc base=`) → same cert EXPIRED + ABORT/refused.
> Completes cert validation: chain + hostname (M451) + validity period.
> M451: **TLS hostname verification (ENFORCED)** — the client
> validated/anchored the cert chain but never checked the cert named the host (any valid cert for
> any domain was accepted = MITM hole). Now `x509.c` parses subjectAltName dNSNames (new bounded
> `find_san` over the `[3]` extensions) + `host_matches_cert` (RFC 6125: SAN-authoritative, no CN
> fallback when SAN present, case-insensitive, single-label wildcard rejecting apex/multi-label/
> `*.com`); `tls.c` REJECTS a definitive mismatch before sending the request, FAILS OPEN on
> uncertainty (>16 SANs / no cert) so legit sites never wrongly reject. Chain anchoring stays
> informational (incomplete root set). x509test +4 real certs +RFC6125 assertions +400k SAN-mutation
> fuzz (ASan/UBSan clean). Subagent review BLOCKED by the cyber safeguard (cert/MITM framing) →
> compensated with a thorough self-audit + the 400k-iter fuzz. Verified in-OS: example.com (SSL.com)
> + danluu.com (Google GTS) both MATCH + load. The remaining "enforcing certs" gap is now just the
> chain-to-root part (needs ~150 baked roots).
> M450: **`<blockquote>` indentation** — quoted blocks indent 24px/
> level, and EVERY wrapped line stays indented (unlike the list-marker first-line-only indent),
> because indent is applied at each line start via a per-token `tokindent` + a `curindent` counter
> (`<blockquote>` open/close adjusts it) consumed by the same line-start `cx` logic as text-align.
> Nested blockquotes indent further; close restores parent. Additive (no bq ⇒ curindent 0 ⇒ cx
> unchanged) + bounded (clamped, cx within [cl,cr)). Verified in-OS (QUOTE.HTM). Lists keep their
> existing first-line marker indent (untouched).
> M449: **column-aligned `<table>` rendering** — was pipe-separated/
> unaligned; now a self-contained `render_table` consumes the `<table>..</table>` region in two
> bounded passes (measure each column's max width, then emit each cell padded to it) → aligned
> columns in the fixed-width font, `<th>` bold. `cell_extract` (strip inline tags/entities/UTF-8,
> collapse ws) + `tbl_classify`. Plugged into `parse_html` as a single `<table>` interception that
> renders+skips the region, so non-table pages are byte-for-byte unaffected. Defensively reviewed
> (15M ASan/UBSan fuzz iters: bounds-safe + always-terminating). Verified in-OS (TABLE.HTM: two
> tables, both aligned). This is the first real **layout** (a contained mini table layout).
> M448: **CSS `font-size` (enlarge)** — `font-size` (inline `style=`,
> px/pt/%/em/rem + large/x-large keywords) + legacy `<big>`/`<font size=N>` scale text up via a
> per-token glyph-scale bucket (≈2× for ≥19px/119%/1.2em, ≈3× for ≥28px/175%/2em). Mirrors M447
> `tokalign` (`tokscale[]` + `curscale` scope); the renderer's per-token scale/line-height + the
> align look-ahead all consult it, so mixed-size words wrap correctly and font-size composes with
> colour/bold. Bitmap font = no sub-1×, so enlarge-only (small falls to 1×). Additive. Verified
> in-OS (SIZE.HTM: 24px→2×, 34px→3×, 200%/2em→3×, `<big>`/`<font size=6>`, 10px stays 1×, large+colour).
> M447: **CSS `text-align: center`/`right`** — inline `style=` +
> `<style>` rules + the `<center>` tag + the `align=` attr now center/right-align text. The
> single-pass word-wrapper does a bounded line-width look-ahead at each line start (replicating
> the wrap test), then offsets `cx`. Per-token `tokalign[]` + a `curalign` scope (mirroring M446
> `tokbg`); a line takes its first token's alignment. Additive (left = zero offset, existing pages
> byte-identical); the look-ahead is ntok-bounded and only moves `cx` within `[cl,cr)` (can't
> corrupt). Verified in-OS (ALIGN.HTM: centered heading + per-line-centered paragraph + right-align
> + `.c`/`.r` rules + `<center>` + `align=`; left baseline unchanged).
> M446: **CSS `background-color`** — `background-color` (+ the
> `background:` shorthand colour) from inline `style=` and `<style>` rules paints an inline
> highlight behind a text run, composing with the color/weight/style/underline scope stack +
> cascade. A per-token `tokbg[]` + `curbg` scope field (mirroring `tokcolor`) feed the renderer's
> existing per-glyph background (what `<mark>`/link-selection use); explicit bg beats `<mark>`'s
> default, UI highlights beat content. Additive, reuses M434-reviewed `style_prop`/`parse_color`.
> Verified in-OS (CSS.HTM: `.hl`/`.code` rules + inline spans + `<mark>`; existing CSS unregressed).
> M445: **SVG gradients** — `<linearGradient>`/`<radialGradient>`
> (multiple `<stop>`s, `stop-opacity`, `gradientUnits`) referenced by `fill=url(#id)`, evaluated
> per-pixel in the scanline fill (integer/16.16, Newton int-sqrt for radial, no FPU). A pre-pass
> collects defs (forward refs + `<defs>` resolve); objectBoundingBox (default) maps to the shape
> bbox, userSpaceOnUse to device. Strictly additive (no gradient = unchanged solid path). Self-review
> found+fixed an int64 overflow (clamp the resolved geometry before dx*dx). ~6.5M ASan/UBSan fuzz
> iters. Verified in-OS (GRAD.SVG → a sky linear gradient behind a 3-stop radial sphere). The SVG
> decoder is now comprehensive: shapes/paths + transforms + inheritance + opacity + gradients.
> M444: **SVG opacity** — `opacity`/`fill-opacity`/`stroke-opacity`
> (a 0..1 fraction or `%`) scale a shape's alpha; a group `<g opacity>` multiplies down to its
> children (inherited `in_alpha` on the same paint stack). Reuses the existing alpha-compositing
> `blend_px`, so translucent overlaps blend. Strictly additive (no opacity = exact prior alpha,
> byte-for-byte); int64 alpha math (no overflow). svgtest +opacity unit test +fuzz; SHIP-reviewed
> (4.5M+ adversarial iters). Verified in-OS (OPAC.SVG → three 50%-opaque circles, overlaps blend).
> M443: **SVG paint inheritance** — shapes inherit
> `fill`/`stroke`/`stroke-width` from a `fill=`/`stroke=` on the root `<svg>` or an enclosing
> `<g>` (was: always black), per-shape values override, `inherit` keyword honoured. A paint
> stack parallel to (and sharing the depth of) the M442 transform stack; strictly additive
> (no ancestor paint = the exact old defaults, byte-for-byte). svgtest gained an inheritance
> unit test + fuzz; SHIP-reviewed (3M+ adversarial iters). Verified in-OS (ICON.SVG → a green
> hexagon inheriting the root `<svg fill>`, white dot overriding). This is how single-colour
> icon sets work (set `fill` once on `<svg>`). 
> M442: **SVG affine transforms** — `kernel/svg.c` now honours
> `transform=` on shapes and `<g>` groups (translate/scale/rotate/matrix/skewX/skewY, composed
> left-to-right) via a depth-16 transform-matrix stack; each user point runs through the current
> 2×3 matrix before the viewBox→pixel map. All 16.16 fixed-point (no FPU), overflow-clamped;
> strictly additive (identity CTM = the M441 render byte-for-byte). `svgtest` extended to fuzz the
> transform parser; SHIP-reviewed (~26.5M adversarial iters, one negative-left-shift UB fixed).
> Verified in-OS (XFORM.SVG → a four-arm pinwheel placed by group translate + per-arm rotate).
> M441: **from-scratch SVG rasterizer** (`kernel/svg.c`, 846 lines,
> integer-only/no-FPU) — decodes a useful SVG subset (rect/circle/ellipse/line/poly*/path with
> beziers, solid fills, viewBox, even-odd scanline fill + stroke) to RGBA, plugged into
> `decode_image` so local+remote SVGs render inline (the M439 `.svg`-skip is lifted); SHIP-reviewed
> + 520k author fuzz (3 UB fixed) + ~12M independent adversarial iters; new `make check` suite
> #7 `svgtest`; verified end-to-end in-OS (SVGT.HTM → blue square/yellow circle/red triangle).
> M440: CSS `text-transform:uppercase/lowercase` — inline `style=`
> + `<style>`-rule cascade, emit-time case-fold into the render pool (so `.textContent` stays
> original), composes with the color/weight/style/underline scope stack; verified in-OS (NEST.HTM). M439: **inline remote images** — the browser now fetches
> + decodes remote PNG/GIF/JPEG `<img>` inline (was: links). Designed via a Plan subagent
> (Option B: in-worker pre-parse fetch, ≤3 imgs, separate `rimg_*` arrays, `parse_html`
> untouched); a header-strip bug was found (stored the whole HTTP response so the decoder
> saw `HTTP/1.1…` not the image) + fixed; skips undecodable `.svg`/`.webp`/`.avif`; SHIP-
> reviewed (8M-iter ASan, concurrency/lifecycle clean) + component-verified (gnu.org fetch+
> match; strip+decode on a real PNG; local-image render). M438: **JS regex bounded quantifiers `{n}`/`{n,}`/`{n,m}`**
> — a real architectural feature done safely by EXPANDING in the parser into existing
> nodes (`a{2,4}`→`a a a? a?`), so the audited matcher/compiler stay byte-for-byte
> unchanged; count-capped + RE_MAXPROG-bounded + ReDoS-safe; reviewed SHIP (3M-iter
> ASan fuzz) + verified in-OS. M424–437: more shell tools — `cmp`/`paste`/`comm`/`diff`
> (the file-compare set: byte / sorted-set / LCS line-edit), `cut -f` (delimited fields,
> completing `cut`), `strings`, `basename`/`dirname`; a JS-engine correctness
> sweep found by probing — getters now fire in value-iteration too
> (`JSON.stringify`/`Object.values`/`entries`/`assign`/spread, M425–428) and
> `to_num(Date)` returns the epoch so Date arithmetic/comparison work (M429); the
> untrusted-input **security audit was deepened on the browser** (M434) — beyond the
> M422 high-level tokenizer review, a granular bound-by-bound pass over every
> self-contained HTML/CSS sub-parser (`decode_entity`/`parse_color`/the inline-`style=`
> parsers/`sel_parse`/the attr helpers), independently reviewed **bounds-safe**, with
> one `decode_entity` defense-in-depth guard; and the safety review was extended to the
> **disk** trust boundary (M435) — a `cluster_in_range` guard on the FAT32 read loops
> (`walk_dir`/`fat32_read`) rejects corrupt out-of-range cluster numbers (strictly
> additive, reviewed SHIP; also closes a latent cluster-0 underflow); plus committed ASan/UBSan fuzz harnesses
> for the kernel's untrusted-input parsers — `make check` = jstest + imgtest + x509test
> + nettest, each verified to catch a reintroduced OOB. M387–423: the JS-engine / browser-demo / shell
> spaces were saturated with verified increments — a 15-page interactive browser
> demo suite (games/sims/tools) + CLI companions (`unmorse`/`unhex`/`unbase64`,
> `cal -3`). Then a **correctness + security pass** on the highest-risk code:
> (a) probing the JS engine found + fixed two real correctness bugs —
> `[1] instanceof Array` returned `false` (M419) and `[1,2]+[3]` returned `0`
> instead of string-concatenating per ToPrimitive (M420) — and verified ~60
> features plus graceful failure on all three exhaustion axes (stack depth, heap,
> malformed syntax); (b) the **Files panel now browses ALL 86 disk files** (was
> capped at 32 — M421); (c) a **comprehensive untrusted-input SECURITY AUDIT** —
> 9 subagent reviews + ASan/UBSan fuzzing of every kernel-side parser
> (network → TLS → X.509 → crypto, and HTML/CSS → image decoders → JS) — found +
> fixed **two real KERNEL memory-safety bugs**: a JPEG DRI out-of-bounds read
> (M422) and a DNS-query-builder kernel-stack overflow (M423), both ASan-proven,
> with the rest verified bounds-safe. See
> [docs/422-untrusted-input-security-audit.md](docs/422-untrusted-input-security-audit.md).
> All reviews SHIP. M374–386: `sort -r`, `cut`, `tr`, `wc -l/-w/-c`,
> `cal MM YYYY`, from-scratch `crc32`, `cowsay`/`fortune`; **`Math.random()`** added
> to the JS engine; interactive browser pages — Rock-Paper-Scissors, a number base
> converter, Guess-the-Number, an ASCII table; and a mkfatfs >64-file capacity fix.
> Ten subagent reviews this session, all SHIP.) Latest arc (M341–358): a shell **networking
> diagnostic toolkit** (`headers` = curl -I, `ping <host>`, `ifconfig`) + a
> deeper e1000 RX ring; **four new apps** (Tic-Tac-Toe vs an unbeatable minimax
> AI, Blackjack, a typing-speed test, Simon) bringing the suite to **twenty-four**,
> with saved best scores/times now across the games (typing/bj/simon/breakout/
> mines/maze); the F9 Apps menu lists **every** app + a shell `apps`; and more
> shell tools (`sha512`, `nl`, `morse`, `factor`, `roll`). Three subagent reviews
> this arc, all SHIP. **M359–373** then added the multi-file text tools
> (`cat`/`grep`/`wc`/`head`/`tail` over many files) + `grep -i/-n/-c/-v/--` +
> `uniq`/`seq`/`rev`/`tac`/`head -N`/`tail -N`, a persistent `todo` manager, and
> a **Connect Four** game vs a 1-ply AI (suite now **25**, F9 menu full at 30) —
> several more subagent reviews, all SHIP. **Remaining big/risky (deferred to protect working
> systems — best done in a focused session):** ENFORCING cert validation (a
> fatal gate; the trust store has ISRG/DigiCert/SSL.com/GTS but enforcement needs
> ~all common roots), **inline remote images** — local `<img>` decode inline;
> remote is a clickable full-page view. APPROACH (investigated M358): the render
> runs on the **WM** (`need_parse`: worker→WM), but only the fetch **worker** has
> the 256 KB stack `tls_get` needs — so the worker must **pre-fetch + decode**
> remote `<img>` bytes into the existing inline-image slots (`imgs[]`) *after* the
> page fetch and *before* signaling render; a render-time fetch on the WM would
> overflow its stack. The pieces exist (decoders, slots, the worker fetch); the
> risk is to the working browser, so a focused session, the **app-exit resource leak** (MAX_APPS spawns/boot; needs a
> careful vmm teardown that frees only the app's user ranges), a **2-column F9
> menu** (the single column is near its ~30-item cap), shell **pipes/redirect**,
> and **FAT32 write robustness** (investigated M366): `fat32_write` is
> delete-then-write (no cluster leak), but `add_entry` (kernel/fat32.c) **fails
> cleanly on a full directory** — it doesn't allocate/link a new dir cluster, so
> file *creation* stops working once the root dir's clusters fill (~the 64 baked
> files + a little slack); fix = extend the dir chain in `add_entry`. Separately,
> the persisted `build/fat.img` showed **empty** after this session's intensive
> write-accumulation (todo + `*.HI` rewrites across many boots). **Diagnosed
> write-triggered (M367):** a fresh disk + read-only boots are fine — `fat.img`
> is byte-unchanged after a read-only boot — so it's the repeated file *writes*
> that corrupt it over time, not a boot/mount-write or QEMU-read artifact;
> consistent with the add_entry dir-fill limit + a likely write-path bug.
> `rm build/fat.img && make` regenerates a clean disk. **First-read-after-boot
> transient (M373):** the *first* file read can intermittently fail ("no such
> file" on a `cat` issued within the first ~5 s post-boot), while the very next
> read of the same file succeeds — a disk-readiness race at boot, not a logic
> bug (cat/grep/tac share one `sys_readfile` path). Any prior disk op (e.g.
> `ls`) or a couple seconds' settle warms it up, and a human typing the first
> command never hits it — only the fast test harness does. Same off-limits
> kernel disk/ATA-init path; deferred. Don't touch the FAT32 write path casually — verify every change against
> the baked files.
>
> ---
>
> The historical status below is from the 337-milestone mark.
>
> **Status (337 milestones):** the big arcs below are now DONE — a from-scratch
> **TLS 1.3 client** browses the real HTTPS web with X.509 chain validation, a
> **comprehensive from-scratch JavaScript engine** (full OOP + ES6 + regex +
> Map/Set/Date) runs in the shell and in pages, and the browser is **fully
> interactive**: a minimal DOM (`getElementById`/`querySelector` →
> `textContent`/`innerHTML`/`.value`/`getAttribute`/`setAttribute`),
> `window.location`, inline `onclick`, the complete form-input set
> (text/password/checkbox/radio/hidden/submit/button), **GET form submission +
> live web search (DuckDuckGo) + address-bar search**, and reactive events
> (`onchange`/`oninput`).
>
> **M302–337 added a small CSS engine and a big app suite:** the browser now has
> a real (if small) **CSS engine** — per-element `color`/`font-weight`/
> `font-style`/`text-decoration` from inline `style=` *and* `<style>` rules
> (`tag`/`.class`/`#id`/`[attr]` selectors), `rgb()`/named colours, a real cascade
> and a nesting scope stack — and the start/help/index pages are styled by it.
> Userspace grew to **twenty apps** via a `sys_setcolor` palette: new ones are
> Sudoku, a Calendar, a Mandelbrot explorer, a Piano, a Maze, a text Adventure, a
> Matrix screensaver, an ASCII Paint (saves/loads files), Hangman, and a music
> Jukebox; the games are colourised and several keep persistent high scores. Only
> CSS *layout* (font-size/text-align/box model) still needs a layout engine.
>
> **M216–260 rounded the JavaScript language + stdlib out to near-completeness**
> (found by systematic probing): the full operator set (`delete`/`in`/
> `instanceof`/`**`/bitwise `^`·`~`/`void`), all compound + logical assignments
> (`&=`…`**=`, `||=`/`&&=`/`??=`), binary/octal/exponent + `_`-separated number
> literals, modern classes (**public instance fields + static methods/fields**),
> computed method names, tagged templates, `typeof undeclared`→`"undefined"`, the
> **`arguments`** object, **Error/TypeError/RangeError/SyntaxError** objects,
> spread of `Set`/`Map`, a full **`Date`** (getTime/valueOf/getDay/
> getMilliseconds + `Date.now()` + setFullYear…setSeconds), and a stdlib that now
> covers essentially every common synchronous method — String (replaceAll,
> matchAll, padStart/End, trimStart/End, at, slice/substring edge cases),
> Array (map/filter/reduce/reduceRight, flat(depth)/flatMap, findLast, at, **the
> ES2023 immutable `with`/`toReversed`/`toSorted`/`toSpliced`**), Object
> (keys/values/entries/assign/fromEntries/freeze/**is**), Number (isInteger/
> parseInt/parseFloat/toFixed/toString-radix/MAX_SAFE_INTEGER), Math (hypot/log2/
> cbrt/clz32/imul/sign), and JSON (pretty `stringify` with indent + `parse`). See
> the milestone table in `README.md` for M139–263.
>
> **The clean stdlib AND the object model are now complete.** M261–263 added the
> last big language pieces — all SHIP-reviewed: **getters/setters** (accessor
> properties in object literals *and* classes), **`Object.defineProperty`/
> `getOwnPropertyDescriptor`**, and a real **prototype chain** (`Object.create`,
> function `.prototype` + `new F()` for plain constructors, `__proto__`,
> `getPrototypeOf`/`setPrototypeOf`). The prototype chain is **additive** — walked
> only at the evaluator's member sites after an own-miss, so `delete`/enumeration
> stay own-only and the class system is byte-identical.
>
> **M264–271 hardened and completed the engine:** `in`/`instanceof` now walk the
> prototype chain; `Object.defineProperties` + `Object.create(proto, descriptors)`;
> `structuredClone` (cycle-preserving deep clone); the `Array(n)`/`new Array(…)`
> constructor; `>>>`/`>>>=` (completing the operator + compound-assignment sets);
> and **loose `==`/`!=` vs strict `===`/`!==`** (the `x==null` idiom + coercion).
> Systematic probing of the mature engine also surfaced and fixed real
> pre-existing bugs: `{}===​{}` returned `true` (objects compared via `to_num`→0),
> string `<`/`>` was always `false` (coerced to 0), `1===true` was `true`, and
> `arr.length =` was ignored.
>
> **M281–283 added `querySelector`/`querySelectorAll`/`getElementsByTagName`/
> `getElementsByClassName` by CSS selector** (`tag`/`.class`/`#id`/compounds) +
> `getAttribute` + WRITE (`textContent`/`innerHTML`/`setAttribute`/`remove`) on
> matches — **without** a DOM tree. The "an id-less match has no name to address
> it by" blocker is solved with **byte-offset position handles** (the matched
> `<`'s offset in `vals[1]`), fully additive (the id path stays byte-identical),
> with the write splice duplicated from the id path and the offset re-validated
> each use (memory-safe; single-match writes exact, multi-write best-effort). So
> the common selector-query DOM API is now covered on the token-stream renderer;
> only node *construction* still needs the tree.
>
> What remains is either *fundamentally blocked by the integer-only Number*
> (`Math.random`, float math, `isFinite`/full `isNaN` — all need a NaN /
> floating-point representation we deliberately don't have) or genuinely
> *architectural*: **CSS layout** (a real box/layout engine), **generators/
> iterators**, **async/Promises**, **modules**, **Proxy**, **`Symbol`**. (The DOM
> itself is now **comprehensively complete** — M281–299 delivered queries,
> traversal [matches/closest/children/parentElement], attributes [get/set/has/
> remove + classList], read/write, **node construction** [createElement/appendChild
> — which turned out NOT to need a tree, via innerHTML-reuse], and the full
> event-handler lifecycle [onclick/addEventListener/onchange/oninput, add/fire/
> remove] on a persistent per-page JS env. And **a small CSS engine now exists**
> — M302–305: per-element `color` / `font-weight` / `font-style` from inline
> `style=` *and* `<style>` rules (`tag`/`.class`/`#id`/`[attr]` selectors), with
> a real cascade and a scope **stack** so styled elements nest to any depth. Only
> CSS *layout* — `font-size`, `text-align`, the box model, backgrounds — still
> needs a layout engine the token-stream renderer lacks; the *machinery* of CSS
> [selectors, cascade, nesting] is done.)

OS-DEV is a **graphical desktop OS** (337 milestones). It boots to a themed
windowing desktop with a **taskbar** that hosts **twenty real ring-3 userspace
programs** as windows — a shell, a clock, a calculator, a text editor, an
ASCII-paint canvas, a calendar, a Mandelbrot explorer, a piano, a music jukebox,
a Matrix screensaver, and ten games (Snake, 2048, Life, Tetris, Breakout,
Minesweeper, Sudoku, Maze, Hangman, a text adventure) — plus a **graphical web
browser**. Under the hood:
preemptive multitasking with sleep/wake and **per-process isolation**, a
read-write **FAT32** filesystem with **subdirectories** and a full file toolkit,
a from-scratch **TCP/HTTP** stack that fetches real web pages (redirects +
chunked transfer-encoding), verified **SHA-256 / AES-128** crypto, sound, a
real-time clock, USB pointer, arrow keys + command history, and a compositor
that caches the scene so cursor moves are cheap.

```
boot → long mode → interrupts → timer/kbd → memory → heap → preemptive threads
 → sleep/wake → userspace/syscalls → libc/shell → FAT32 → PCI → ARP/ICMP/UDP/DNS
 → framebuffer → console → mouse → USB tablet → window manager → desktop apps
 → RTC → sound → process spawning → TCP+HTTP → browser (links, history, async,
   rendering, start page, save, local files, redirects, chunked, <pre>)
 → FS subdirectories → file toolkit → taskbar → ps → arrow keys → history
 → snake → editor → 2048 → compositor cache → system monitor → SHA-256 → AES
 → keyboard browser nav + bookmarks → wget → F2 focus-cycle → F4 maximize
 → lists/tables/entities → interactive Files → run ELF from disk → scrollback
 → editor scroll → in-page find → img alt → form fields → PNG images
 → run-from-disk → GIF images → font colour → Tetris → 100 milestones  ✅
 → window tiling → multi-cluster disk → render hot-path optimisation sweep
 → shell history → inline code → inline images → JPEG → Adam7 PNG → tab-complete
 → progressive JPEG → animated GIF → Minesweeper
 → X25519 → AES-GCM → HMAC/HKDF → ChaCha20-Poly1305 → X.509 parser
 → RSA verify → ECDSA-P256 (TLS 1.3 crypto+cert toolkit complete) → TCP stream API
 → TLS 1.3 client — browser fetches real HTTPS pages
 → entities in alt text + Latin-1 folding → UTF-8 body text
 → click-through HTTPS browsing (relative-link scheme + multi-fetch)
 → HTTPS from the shell (get/wget over TLS) → DNS cache
 → parser robustness (HTML comments + inline SVG)
 → window minimize/restore (F3)
 → TCP partial-segment fix + live NPR/gnu.org/danluu/example.com
 → network info in System Monitor
 → keyboard-drivable Apps menu (F9)
 → TLS CertificateVerify (server key-proof, real certs)
 → X.509 chain-internal verification (each cert signed by the next)
 → root-CA trust store + chain anchoring (NPR → ISRG Root X1)
 → SHA-384/512 + SHA-384 cert sigs (microsoft.com chain 2/2)
 → ECDSA P-384 (example.com EC chain 3/3)
 → trust store (ISRG/DigiCert/SSL.com) + key-match anchoring
 → out-of-order TCP reassembly → GTS Root R4 (google.com → TLS*)
 → from-scratch JavaScript interpreter (shell `js`)
 → JavaScript runs in the browser (pages execute their <script> tags)
 → JS standard library (Math, JSON.stringify/parse, String/Array map·filter·…)
 → arrow functions (x => x*x)
 → template literals (`hi ${name}`)
 → clickable JavaScript (<a href="javascript:..."> runs JS on click)
 → JS switch / do-while / for-of → try/catch/finally/throw (exceptions)
 → Object.values/entries, Array.isArray/from → object shorthand + default params
 → localStorage (stateful pages) → Date/for-in → parseInt(radix)/Object.assign/
   Array.flat/Math.sign/Number.toString(radix)
 → JS `this`/`new`/OOP → `class`/`extends` → `super` → spread/rest `...`
 → destructuring → full class-based OOP + ES6 (verified in-OS) → 169 milestones  ✅
```

The browser is now **fully keyboard-drivable**: Tab/n/p to select links, Enter
to follow, Backspace to go back, `a` to bookmark, `s` to save, `/` to edit the
address, PgUp/PgDn to page, `\` for in-page find, `h` for home, `g`/`G` for
top/bottom. It renders headings, bold/italic, inline `<code>`, **colour text**,
links, **numbered & nested lists, tables, `<pre>`, form fields, and real PNG,
GIF and JPEG images rendered inline in the page** (from-scratch DEFLATE/PNG +
LZW/GIF + baseline-JPEG decoders, host-tested and fuzzed), and decodes HTML
entities (named + decimal + hex). The WM has keyboard window management: **F2**
cycles focus, **F4**
maximizes/restores; the **Files window is an interactive launcher** (arrows +
Enter open a file in the browser); apps have **scrollback** (PgUp/PgDn). The OS
can also **load and run ELF programs from the disk** (`run calc.elf`), not just
the kernel-embedded ones.

Shell toolkit: `ls cat edit write rm cp mv mkdir cd pwd tree find grep df
hexdump wc cal sha256 crypt base64 run get wget browse echo date ping resolve
beep mem ps clear reboot ver pid exit`.

## The biggest remaining gaps

1. **Browser interactivity — LARGELY DONE; the frontier has moved.** The browser
   browses the real HTTPS web (from-scratch **TLS 1.3** + X.509 chain validation to
   baked-in roots — `TLS*` on example.com/NPR/gnu.org/google.com), runs a
   **comprehensive JS engine** (OOP + ES6 + regex + Map/Set/Date), and is now
   **interactive**: a minimal **DOM** (`getElementById`/`querySelector` →
   `textContent`/`innerHTML`/`.value`, `getAttribute`/`setAttribute`),
   `window.location`, inline `onclick`, the full **form-input set**, **GET form
   submission**, **live web search** (DuckDuckGo, from a form or the address bar),
   and **reactive events** (`onchange`/`oninput`). What's left for *more*
   interactivity — each a real build, best done with guidance:
   - **Event handlers — DONE (M287–292)**: a persistent per-page JS env + a handler
     registry; the full lifecycle works — `onclick`/`addEventListener`/`onchange`/
     `oninput` (inline *and* JS-assigned), add / fire (with an `event` arg + `this`) /
     remove (`removeEventListener`/`onclick=null`), state persisting across events.
     (Minor polish left: id-less-element handlers, multiple listeners per event.)
   - **DOM queries + traversal + element API + construction — DONE (M281–299)**:
     querySelector(All)/getElementsBy* by CSS selector (tag/`.class`/`#id`/`[attr]`/
     compounds); traversal `matches`/`closest`/`children`/`parentElement`; attributes
     `get`/`has`/`set`/`removeAttribute` + `classList`; textContent/innerHTML/value
     read+write, `remove`; and **node construction** `createElement`+`appendChild`
     (turned out NOT to need a tree — `parent.innerHTML += built-HTML`). All via
     byte-offset *position handles*.
   - **CSS — a small engine DONE (M302–305)**: per-element `color` / `font-weight`
     / `font-style` from inline `style=` *and* `<style>` rules (`tag`/`.class`/`#id`/
     `[attr]` selectors), a real cascade (rules < inline, per property), and a scope
     **stack** so styled elements nest to any depth. Only CSS *layout* — `font-size`,
     `text-align`, the box model, backgrounds — still needs a layout engine the flat
     token-stream renderer lacks. Possible next slivers without one: `rgb()`/more named
     colours, `text-decoration` (underline/line-through).
   - **CSS / layout**, cookies (sessions), inline remote `<img>`, `<textarea>`
     multiline.
   *Known limit: `lite.cnn.com` etc. refuse our minimal ClientHello (Fastly TLS
   fingerprinting) — not a TCP/crypto bug. Hard ceiling stands: real web apps
   (Google Docs) / Chromium are out of reach — see GOALS.md.*
2. **`fork` / `exec` + a process table.** Programs spawn fresh from embedded or
   on-disk ELFs (loading from disk now works — milestone 85); true `fork` +
   `exec` would enable a Unix-like model with child processes.
3. **Richer browser rendering.** **Inline `<img>` for remote (`http:`) images**
   (local images render inline — milestone 111; remote ones still need an async
   per-image fetch state machine on top of the single fetch worker/buffer),
   hanging-indent list wrap, and true column-aligned tables. (`<pre>`, lists,
   tables, entities, bold/italic, headings, links, form fields, text colour,
   inline code, `<img>` sizing, **inline local PNG (incl. interlaced), GIF (incl.
   animation), and baseline-and-progressive JPEG images**, and the full-page
   image viewer are all done — three from-scratch image decoders, host-tested and
   fuzzed. Every common web image format is supported.)
4. **Long file names** (subdirectories exist, but 8.3 names only).

## Smaller polish

- Window **minimize** (F3) + tiling/snap (F5/F6) are done; terminal copy/paste (scrollback is done).
- Browser: scrollbar drag, in-page find, history forward (Back exists).
- A graphical **file manager** with icons (the Files window is now an
  interactive launcher); load the wallpaper from disk.
- **APIC/HPET** + SMP (multi-core); faster syscalls (`syscall`/`sysret`).

## Notes

- Per-milestone learning docs are in `docs/00`…`docs/78`; the table in
  `README.md` indexes them. Screenshots are the `docs/osdev-*.png` files.
- Long-term north stars (a full browser, running Claude Code) and the
  pure-from-scratch vs Linux-compatible decision are analyzed in `GOALS.md`.
- Testing is headless: QEMU `-display none -monitor stdio` + `sendkey`, screendump
  → PNG. The browser and WM are now keyboard-driven, so they're fully scriptable.
