# What's next

> **Status (492 milestones).** (M492: **`unzip` — extract `.zip` archives** — completes the archive story
> (.gz single → .zip multi-file). kernel/zip.c parses via the CENTRAL DIRECTORY (authoritative index),
> reuses `inflate` (zip method 8 = raw DEFLATE; 0 = stored), bounds-checks EVERY attacker-controlled
> offset/size in 64-bit (no wrap). DELEGATED to a subagent (intricate format + fuzzing, non-cyber) →
> reviewed (in_bounds on all reads, EOCD backward-scan bounded, central-dir walk capped by count AND span,
> inflate output bounded by scratchcap) → integrated. SYS_unzip(40) + a kernel emit callback that 8.3-
> mangles each path + vfs_writes; shell `unzip <f.zip>`. New ziptest in `make check` (exact extraction incl.
> a system-`zip` archive w/ comment+subdir + truncation/corruption fuzz, ASan clean). Verified in-OS:
> `unzip TEST.ZIP` → 3 files extracted byte-exact (stored + a 2400-byte deflate entry, confirmed via disk).
> Files: kernel/zip.c, zip.h, syscall.c, shell.c, ulib.*, Makefile, tools/test.zip, tests/zip/*.
> **10 host suites now. Compression/archive arc complete: gunzip + gzip + unzip + PNG, all from-scratch,
> all reusing the inflate/deflate core. Three intricate algorithms (deflate/png/zip) via subagents +
> my review this session — exactly the directive's "use subagents for hard work + run reviews".**)
> M491: **PNG screenshots — from-scratch PNG ENCODER** — kernel/png_encode.c
> (filter-0 scanlines → zlib[raw_deflate body + Adler-32] → IHDR/IDAT/IEND chunks + CRC-32), fully bounded.
> DELEGATED to a subagent (intricate, non-cyber) → reviewed (every out-write via bounded pe_put, IDAT
> length-patch + CRC guarded by the oom check first, integer-overflow guarded) → integrated. fb_save_png
> (capture top-down RGB → png_encode, transient kmalloc bufs). F12 now saves SHOT0.PNG/SHOT1.PNG (~9KB vs
> ~576KB BMP = 64x smaller — the screen compresses well); shell `screenshot <f>` → PNG for a .png name else
> BMP (SYS_screenshot dispatches by extension). New pngenctest in `make check` (round-trips vs the DECODER,
> exact RGB, ASan/UBSan, bounds) + subagent libpng/`file` interop. Verified in-OS: F12 → SHOT0.PNG (PIL
> opens it 512x384 RGB, 9173 bytes) → `browse file:SHOT0.PNG` renders via the OS's OWN png_decode = full
> from-scratch encode→decode round-trip. Files: kernel/png_encode.c, fb.c, fb.h, syscall.c, desktop.c,
> Makefile, tests/png/*. **Capstone: ties M490 deflate + M478-480 image work + the PNG decoder together.
> 9 host suites now. Both M490/M491 substantial from-scratch algorithms via subagents + my review.**)
> M490: **`gzip` — from-scratch DEFLATE COMPRESSOR** — completes the
> compression story (had gunzip/decompress, now compress). New kernel/deflate.c (encoding counterpart to
> inflate.c): LZ77 (3-byte hash-chain, 32KB window, MAX_CHAIN=128 anti-blowup) + fixed-Huffman + CRC32,
> fully bounded (every write vs outcap). `gzip <file> [out.gz]` + SYS_gzip(39). DELEGATED to a subagent
> (hard/intricate, NON-cyber so not blocked) then I reviewed (bit-ordering mirrors inflate's reader,
> canonical codes built via the §3.2.2 procedure so they can't drift from the decoder, LZ77 reads proven
> in-bounds) + integrated. New `deflatetest` suite in `make check`: round-trips vs the decoder over
> empty/repetitive/random/text ≤200KB (ASan/UBSan clean) + outcap-bounds + **system `gunzip -t` interop**
> (proves CRC/format real). Verified in-OS: `gzip README.TXT; gunzip README.GZ DEC; cmp` → "files are
> identical" (a full from-scratch compress→decompress round-trip). Files: kernel/deflate.c, inflate.h,
> syscall.c, shell.c, ulib.*, Makefile, tests/deflate/*. **Substantial from-scratch algorithm (per the
> directive: don't decline for complexity; use subagents for hard work). 8 host suites now. Could later
> enable PNG screenshots (smaller than BMP) — deflate is the prerequisite, now done.**)
> M489: **`base64 -d` decode + encode-redirect fix** — the shell's base64 was
> ENCODE-only; added `base64 -d <file> [out]` to DECODE base64→bytes (skips whitespace, stops at `=`/invalid,
> bounded static bufs, b64v helper). ALSO fixed a real pre-existing bug found while testing: `base64 F > OUT`
> failed because the encode passed `readfile` the filename WITH the redirect's trailing space ("MOTD.TXT ")
> → trimmed it now. Together = clean round-trip. Verified in-OS: `base64 MOTD.TXT > ENC.B64; base64 -d
> ENC.B64 DEC; cmp MOTD.TXT DEC` → "files are identical" (78 bytes). File: user/shell.c. **Like M488
> (gunzip), a safe gap-fill — completes an existing half-feature + fixes a latent bug, fully verifiable
> in-OS via the round-trip. Both M488/489 are pure-userspace-or-reuse, zero risk to working systems.**)
> M488: **`gunzip` — .gz decompression** — shell `gunzip <f.gz> [out]` +
> SYS_gunzip(38) decompress a gzip file by REUSING the fuzz-tested DEFLATE `inflate`; only a thin bounded
> wrapper `gz_inflate` (kernel/inflate.c: magic/method check, skip header + optional FEXTRA/FNAME/FCOMMENT/
> FHCRC, inflate body, ignore trailer) is new. SAFE + additive (new syscall+command, no working-system
> touch; kmalloc'd 256KB-in/1MB-out transient buffers). imgtest gained a real gzip round-trip (exact
> 84-byte output) + 120K gzip-header fuzz (ASan/UBSan clean). Baked tools/hello.gz (mkfatfs hostfiles).
> Verified in-OS: `gunzip HELLO.GZ` → wrote HELLO (84 bytes); `cat HELLO` → exact text. Files:
> kernel/inflate.c, kernel/syscall.c, user/shell.c, user/ulib.c, tools/mkfatfs.c, tools/hello.gz.
> **The best safe+valuable find post-saturation: real new capability (.gz) reusing proven fuzzed code,
> fully host- AND in-OS-verified, zero risk to working systems. (.gz fits 8.3, unlike the reverted .json.)**)
> M487: **F12 screenshots auto-number** — F12 now saves SHOT0.BMP, SHOT1.BMP,
> … (a static counter) instead of overwriting SHOT.BMP, so successive captures aren't lost. Zero-risk
> self-contained change to the F12 handler (shell `screenshot [file]` unchanged). Verified in-OS: 3× F12 →
> SHOT0/1/2.BMP all valid, no overwrite. File: kernel/desktop.c. **NOTE: hit the 8.3 FILENAME LIMIT this
> session — a `.json` viewer was written then reverted because a 4-char extension can't be stored on the
> 8.3 FAT disk (only ≤3-char exts: .md/.csv/.htm fit). Long-filename (VFAT/LFN) support would lift this but
> risks the working FAT write path — deferred. Genuine saturation: 10 milestones M478–487 this session
> (screenshot save/view/embed/hotkey, markdown+GFM, CSV, Wordle); remaining items are ceilings or FS-risk.**)
> M486: **Markdown autolinks + strikethrough** — rounds out `md_inline`: a
> bare `http(s)://` URL → a clickable `<a>` (bounded scheme-prefix probe + scan to whitespace/delimiter),
> and `~~text~~` → `<s>` (flat toggle like bold/italic; the renderer already draws a strike-line for STY_
> STRIKE). Both isolated to the inline scanner — non-recursive, bounds-checked, no touch to anything risky.
> readme.md demo gained a bare URL + struck text. Verified in-OS: browse file:readme.md → https://example.com
> is a blue link, struck text is greyed with a line through it. File: kernel/browser.c, tools/mkfatfs.c.
> **The markdown renderer is now fairly complete GFM: headings/bold/italic/strike/code/fenced/lists/quotes/
> links/autolinks/images/tables. Safe + fully locally verifiable.**)
> M485: **CSV files render as tables (`.csv`)** — a local `.csv` opens as an
> HTML `<table>` via `csv_to_html` (same safe pattern as markdown: convert → parse_html, reuses md_put/
> md_esc). RFC-4180 quoting: a `"`-quoted field may contain commas, `""` = a literal quote; first row =
> `<th>` header. Bounded (writes capped, reads within len; rows split on `\n` — embedded newlines in quotes
> unsupported, documented). Additive: only file: URLs ending `.csv`. Baked data.csv demo + index link.
> Verified in-OS: `browse file:data.csv` → table renders, and the quoted field "Designer, UX" stays ONE
> cell (proves quote handling). File: kernel/browser.c, tools/mkfatfs.c. **Rounds out local-document
> support: HTML / text / Markdown / CSV / images. Safe + fully locally verifiable.**)
> M484: **GFM markdown: tables + images** — extends M482's `md_to_html` with
> `![alt](url)` images (emit `<img src>`, flows through the inline-image path → a .md can embed file:/data:/
> remote images, tying the whole image arc together) and GFM tables (a `| a | b |` line + a `|---|` separator
> next line → `<table>` with `<th>`/`<td>`; `md_table_row` treats `|` as a pure delimiter, trims cell spaces).
> Still bounded + non-recursive: the table body consumes consecutive `|`-containing lines then leaves the
> first non-table line for the main loop; every read within the line, every write capped. readme.md demo
> gained a table + an image. Verified in-OS: `browse file:readme.md` (scroll down) shows the table (bold
> header, aligned cols: Feature/Status x Browser/JavaScript/Markdown) + the inline striped test.png image.
> File: kernel/browser.c, tools/mkfatfs.c. **Builds on the freshly-shipped M482 — safe + FULLY LOCALLY
> verifiable (no network), and integrates the image work into markdown.**)
> M483: **Wordle (new game app)** — guess a hidden 5-letter word in 6
> tries; each guess coloured green (right letter+spot) / yellow (in word, wrong spot) / grey (absent) via
> the standard TWO-PASS scoring (greens claim positions first, then yellows from leftovers — so duplicate
> letters score correctly). ~110-word baked list, clock-seeded xorshift PRNG, Enter=new round on game end.
> STRICTLY ADDITIVE (zero risk): a new ring-3 ELF (user/wordle.c) wired into the 5 registration points —
> Makefile USER_ELFS, kernel/asm/user_blob.asm (incbin), kernel/app.c (extern + progs[]), desktop.c menu[].
> `run wordle` or Apps→Wordle. Verified in-OS: launched + focused, accepted typed guesses, scored/coloured
> them (green+grey clearly distinct), guess count 3/6, grid + dot-rows rendered, stable. Uses the colored-
> text app API (sys_setcolor palette: 0=green, 3=yellow, 8=grey) like paint.c. **A new app is the safest
> possible addition — found after confirming images/shell/JS-stdlib/browser-core were saturated; the user
> explicitly wants "as many cool features as you can."**)
> M482: **Markdown rendering in the browser (`.md` files)** — the browser
> renders local Markdown: a from-scratch markdown→HTML converter (`md_to_html` in browser.c) turns a `.md`
> file into HTML, fed to the existing battle-tested `parse_html` (so it inherits all the styling). Handles
> # headings, bold/italic, inline `code`, ``` fences, -/* /+ and N. lists, > quotes, [text](url) links,
> --- rules, paragraphs. BOUNDED + NON-RECURSIVE (untrusted input, no kernel guard page): every write
> capped vs cap, every read bounded by line length, inline emphasis = flat toggles (no recursion). Reuses
> the M479 big-buffer local-file path: read .md into the transient buffer, convert into b->raw, parse_html.
> Additive — only file: URLs ending .md take the path; low-mem fallback = plain text. Baked readme.md demo
> + index link. GOTCHA fixed: the function's doc comment literally contained `*` `/` sequences (in the
> markdown syntax it described) that closed the block comment early — reworded. Verified in-OS:
> `browse file:readme.md` → heading/bold/italic/code/fenced/bullet/quote/link/hr/ordered-list all render
> (status "markdown"). File: kernel/browser.c, tools/mkfatfs.c. **Substantial + safe + on-theme (extends
> the browser north star to a new doc format) — found after confirming images/shell/JS-stdlib/apps are
> saturated.**)
> M481: **Global screenshot hotkey (F12)** — press F12 anywhere on the
> desktop to save the whole screen to `SHOT.BMP` (+ a short beep), so the M478 capture is reachable from
> any focused app, not just the shell. keyboard.c maps F12 (scancode 0x58, was unused) to a new WM code
> 0x1C; desktop.c handles it by calling `fb_save_bmp` directly (kernel-side, no syscall) and swallowing
> the key. Strictly additive (F12 did nothing before). Welcome-window hint + GUIDE.TXT updated. Verified
> in-OS: F12 at the desktop writes a valid 512×384 BMP (extracted from the disk = the full desktop,
> windows/taskbar/cursor, colours right), desktop stays stable. Files: kernel/keyboard.c, kernel/desktop.c.
> **Caps the image arc (M478 save / M479 view / M480 embed / M481 hotkey) — a coherent, complete feature.**)
> M480: **Inline `data:` image URIs (base64)** — the browser now decodes
> `<img src="data:image/...;base64,…">` images embedded in the page, completing the image-source story
> (`file:`/`http:`/`https:`/`data:`). New bounded `b64_decode` in browser.c (every write guarded vs the
> out cap, every read within input — untrusted-safe like the decoders) → feeds the existing inline-image
> slot path, which sniffs format by magic, so ALL five decoders (PNG/GIF/JPEG/SVG/BMP) work from a data:
> payload. Strictly additive: a data: `<img>` used to fall back to a dead `[img]` link, and any parse/
> decode failure still does (graceful); reuses the M479 BMP decoder. Baked `dataimg.htm` demo embeds a
> 16×16 BMP. Verified in-OS: `browse file:dataimg.htm` → red/green/blue/yellow quadrants in the right
> places (proves base64→BMP→RGBA end-to-end), scaled width=128. Files: kernel/browser.c, tools/mkfatfs.c.
> **On-theme (the browser is the crown jewel): data: images appear on the real web; this + M478/M479 form
> a coherent image arc — capture to BMP, view a BMP, decode an embedded BMP.**)
> M479: **BMP decoder — VIEW a screenshot in the browser** — a from-scratch
> decoder (`kernel/bmp.c`) for uncompressed BI_RGB Windows BMPs (24-/32-/8-bit palettized, bottom-up or
> top-down → RGBA, A=255), wired into the browser's `decode_image` dispatch — closes the loop with M478:
> the OS can now both SAVE and VIEW a screenshot. Untrusted-byte safe (every pixel/palette read bounded
> vs `len`, like the other decoders). imgtest gained a 2×2 correctness case (proves bottom-up row-flip +
> BGR→RGBA) + a 120k `BM`-prefixed fuzz (ASan/UBSan clean). KEY: the browser's `file:` path read into the
> 256 KB HTML buffer, too small for a ~576 KB 24-bit screenshot → first attempt rendered the BMP bytes as
> TEXT. FIX: read a local image into a transient 1 MB buffer (`LOCAL_IMG_MAX`), try_image, free; graceful
> fallback to the old capped text/HTML path if it's not an image / alloc fails (protects the working
> local-file path). Verified end-to-end in-OS: `screenshot` → `browse file:SHOT.BMP` renders the captured
> desktop full-page (status "image"), colours correct. Files: kernel/bmp.c, kernel/browser.c. **Genuinely
> useful (not bloat): the BMP decoder works for any BMP — disk files, `<img src>` in pages — not just
> screenshots, and the buffer fix improves local-image viewing generally.**)
> M478: **Screenshot to BMP (`screenshot [file]`)** — new `SYS_screenshot`
> syscall + shell command (default `SHOT.BMP`) saves the live desktop to a 24-bit BMP on the FAT32 disk,
> downscaled 2× (≤512×384, ~576 KB). KEY FIX found in testing: the first cut read the back buffer
> (`target`) and caught it MID-COMPOSE (captured the wallpaper gradient only, windows missing) — switched
> to read the *presented* framebuffer (`lfb`), always a complete composited frame. Emits bottom-up BGR
> rows. Strictly additive (one self-contained `fb_save_bmp` + a syscall; no draw-path change). Verified
> end-to-end: extracted SHOT.BMP from the disk image = a valid 512×384 BMP matching QEMU's own screendump
> pixel-for-pixel (windows, green shell text, blue title bars, colours/BGR-order all correct) + 7 suites.
> File: kernel/fb.c. **(This also incidentally re-confirmed M471 FAT dir-growth in practice — SHOT.BMP is
> the 87th root-dir file, living in an extended dir cluster.)**
> M477: **Glob matcher hardened vs catastrophic backtracking** — a PERIODIC
> REVIEW of the shell command-line arc (M463/M468/M473/M474) found glob_match (M473) was depth-bounded
> (no stack overflow) BUT time-EXPONENTIAL: `*a*a*a*…*b` vs a long non-matching name → billions of calls
> (the `for(;*s;s++) if(glob_match(p,s))` retries the tail at every suffix, no memo) → `cat *a*a*…*b`
> HANGS the shell forever (ReDoS-style, paid per non-matching file). FIX: replaced the recursive matcher
> with the standard ITERATIVE two-pointer (star/mark backtrack) matcher — O(pat×name), no recursion, no
> blowup. Verified in-OS (cat *.txt still globs; `cat *a*a*a*a*a*a*a*a*a*b`→instant "no such file"; echo
> after → shell responsive) + 7 suites. **The review's other 5 items (glob_expand/run_pipe/write_redirect/
> run_line/ulib-capture bounds) were confirmed SAFE — only glob_match had the bug.** Validates running
> periodic reviews (my self-audit checked recursion DEPTH, missed TIME). Same spirit as the engine's
> ReDoS-safe regex. File: user/shell.c.
> M476: **Shell `help` documents the 4 command-line operators** — added
> a line to the `help` text listing `| > >> *.txt ? ;` (pipe/write/append/glob/sequence) so the M463/
> M468/M473/M474 operators are discoverable. Tiny user/shell.c help-text change; build + 7 suites + in-OS
> (`help` shows the syntax lines). **SATURATION NOTE: after 36 milestones this session the achievable
> safe+high-value work is exhausted — remaining items are architectural CEILINGS (real lazy generators
> + CSS layout need a different interpreter/renderer arch), BLOAT/break-risk (fatal cert enforcement
> needs ~all ~140 roots), REGRESSION-risk (gzip-advertise could break proven-working sites + live-verify
> is flaky), or SUBTLER/risky (FAT cross-boot corruption). Cross-app clipboard needs webpage text
> selection for real value (token renderer has none). Keep any further work SMALL+SAFE+verified.**)
> M475: **Expanded TLS trusted-root store (5→13 CAs)** — added 8 common
> roots to kernel/rootca.c's ROOT_CAS[]: USERTrust RSA, DigiCert Global Root CA (G1) + G3 (EC), Amazon
> Root CA 1, GlobalSign R3, GTS Root R1, Sectigo ECC E46, ISRG Root X2 (EC). Each = the root's public key
> in x509_cert.key form (RSA → RSAPublicKey DER via `openssl rsa -RSAPublicKey_out -outform DER`; EC →
> the uncompressed P-384 point parsed from `openssl pkey -pubin -text`), extracted from
> /etc/ssl/certs/ca-certificates.crt by a python script (the SAME pipeline + format as the existing 5
> roots). So the full cert-path validation now ANCHORS (TLS*) for the bulk of the real HTTPS web. SAFE
> by construction: anchoring is INFORMATIONAL (tls.c logs chain_anchored, the gate enforces only
> hostname+validity), and a mis-extracted key just fails to match — it can NEVER falsely validate (no
> regression possible). Verified: build + 7 suites + in-OS (https://example.com still loads + cert-info
> 'i' → "anchored" via SSL.com = no regression; new roots anchor by-construction, same format). Bloat
> ~2.4KB key data. NOTE: still NOT a fatal enforcement gate (would reject un-baked-root sites → break
> risk; full enforcement needs ~all ~140 roots = bloat). File: kernel/rootca.c only.
> M474: **Shell command sequencing (`;`)** — `cmd1 ; cmd2 ; cmd3` runs each
> in turn. Refactor: the per-line dispatch (glob→redirect→pipe→run) moved into `run_line(seg,cwd)`
> (returns 1 only for "exit"); main's loop splits the line on `;`, TRIMS each segment's leading space
> (else a non-piped 2nd segment " echo x" → "unknown command"; run_pipe already trimmed internally so
> the pipe path worked — that's how the bug surfaced), skips empties, run_line's each, breaks on exit.
> `;` binds lower than `|`/`>` (each segment handles its own). Verified in-OS (echo a;echo b;echo c →
> a/b/c; echo a ; ls|grep TXT → both; single cmd unaffected) + 7 suites. **Shell command-line operator
> set COMPLETE: pipes `|` (M463) + redirect `>`/`>>` (M468) + glob `*`/`?` (M473) + sequence `;` (M474).**
> File: user/shell.c only.
> M473: **Shell filename globbing (`*`/`?`)** — the shell expands wildcard
> patterns against the directory before parsing operators: `cat *.txt`, `grep PAT *.htm`, `wc *.svg`.
> `glob_match` (case-insensitive — FAT is 8.3-uppercase; `*`=any run, `?`=one char, recursion bounded by
> the short filename) + `glob_expand` (tokenize line; for a token with `*`/`?`, scan the sys_list listing
> ["NAME size" per line], substitute matches; no-match→literal; bounded into a 1024 buf). Wired in main()
> BEFORE the redirect/pipe parse, but ONLY when the line actually contains `*`/`?` (else cmd=line
> verbatim → no whitespace-collapse for normal commands like echo). Operators |/>/>> pass through (no
> wildcard). Userspace, additive. Verified in-OS (`cat *.txt`→3 files concatenated; `grep Milestone *.txt`
> →MOTD.TXT match; `cat *.zzz`→literal→"no such file") + 7 suites. Shell now has pipes + redirect + glob.
> Files: user/shell.c only.
> M472: **JS iterator protocol — spread + Array.from** — completes M469:
> `[...obj]` (array-literal spread eval, case N_ARRAY) + `Array.from(obj)` now consult a plain object's
> `[Symbol.iterator]` (before, only for-of did — an inconsistency). New `iter_collect(it,dest,mapfn,hasfn)`
> helper (fwd-decl before case N_ARRAY, defined after from_push) mirrors the for-of drive EXACTLY (same
> callable checks, fetch-next-once, SAME 2000 cap + g_err/g_oom guards → untrusted runaway can't hang),
> appends via from_push (so Array.from's map fn applies); returns 1 if iterable / 0 WITHOUT appending
> (safe in an else-if). Array.from's iterable branch precedes the array-like `{length:n}` branch.
> Strictly additive (arrays/strings/sets/maps/array-likes/non-iterables unchanged). Subagent-built, I
> reviewed (iter_collect = faithful copy of the M469-reviewed for-of drive: capped, no-append-on-0,
> bounds-safe) + jstest PASS (custom-iterable spread/Array.from/map-fn + regressions + adversarial cap
> "spreadcap=true") + 7 suites + in-OS (ITER.JS: `[...range]`→"1-2-3-4", Array.from→4). The iterator
> protocol is now CONSISTENT across for-of/spread/Array.from. Files: kernel/js.c, tests/js/suite.*, mkfatfs.c.
> M471: **FAT32 directory growth** — `add_entry` (kernel/fat32.c) now
> GROWS a full directory's cluster chain instead of failing: tracks the chain tail `last`, allocs a
> fresh cluster, zeroes all its slots, writes the entry in slot 0, then `fat_set(last,newcl)` links it
> (write-before-link so a chain-walk never sees an uninit cluster) — the same chain-extension fat32_write
> uses for file data. Fixes the "file creation fails past a full root dir" limitation (deferred issue
> (b)), newly relevant since pipes/redirect create files. Fwd-declared alloc_cluster/fat_set. fstest
> gained a Phase 3: create 100 distinct files (forces ~7 dir-cluster growths) + read every one back
> (ASan/UBSan). SHIP-reviewed (no corruption: tail-tracking, zeroing incl. sec_per_clus==1 edge,
> write-before-link, both FAT copies, walk_dir follows the extended chain, non-full path unchanged) +
> in-OS (25 files via `>`, first+last read back, 0 faults). NOTE: this is the SAFE FAT-write fix; the
> OTHER deferred FAT issue — "heavy repeated writes corrupt fat.img across BOOTS" (issue (a)) — is a
> subtler cross-boot/persistence concern (fstest's in-memory single-run write stress is ASan-clean, so
> it's not an in-run OOB; cause unclear) and STAYS deferred. No rmdir, so a grown dir never shrinks
> (pre-existing). Files: kernel/fat32.c, tests/fs/fs_test.c.
> M470: **JS `Proxy` (get/set traps)** — `new Proxy(target,handler)`;
> property read fires handler.get(target,key,proxy) at eval_member_get, write fires handler.set at the
> assign sites (both depth-guarded via call_function_this); trap-less proxy = transparent target r/w;
> enumeration/JSON/in/delete deproxy to the target; V_PROXY is NOT obj_keyed so vals[0/1] never leak.
> Additive (one is_proxy branch per site; non-proxy unchanged). Arena bumped 16→20MB BSS (suite's no-GC
> peak). **SHIP-review CAUGHT A CRITICAL BUG: the trap-less GET fall-through `return eval_member_get(target)`
> RECURSED, so a nested-proxy chain (new Proxy(aProxy,{})×N, ~250) overflowed the guard-page-less kernel
> stack (untrusted script → kernel corruption; reproduced under ulimit -s 256). FIXED: walk the trap-less
> chain ITERATIVELY (bounded loop, cap 4M >> any arena-buildable chain, so one-pass; stops at non-proxy
> or trap-having → final read recurses ≤1 guarded level). Added a 300-deep-chain jstest case (was
> missing).** jstest PASS (get/set/trapless/enum/typeof/method/self-recursive/deep-chain, ASan/UBSan) +
> 7 suites + in-OS (js PROXY.JS → "got:bar"/"foo=5"/"chain 9", 200-deep chain, 0 faults). Baked PROXY.JS.
> **GENERATORS = the only major modern-JS feature left, and it's a tree-walker CEILING (real lazy
> generators need eval suspension; an eager hack mis-orders side effects) — document, don't force it.**
> Files: kernel/js.c, tests/js/suite.js+.expected, tools/mkfatfs.c.
> M469: **JS `Symbol` + iterator protocol** — new V_SYMBOL primitive
> (typeof "symbol", unique id, equality by id; symbol-keyed props encoded as "@@sym:<id>" + hidden from
> Object.keys/for-in/JSON/etc.), `Symbol`/`Symbol.iterator` globals, and `for…of` now consults a plain
> object's `[Symbol.iterator]()` → CUSTOM ITERABLES work (call it → iterator → loop next()→{value,done}).
> Untrusted-input safety: the for-of loop is hard-capped (FOROF_ITER_MAX=2000, CATCHABLE rt_err) +
> g_err/g_oom-guarded each step + each next() is depth-guarded — can't hang/overflow the kernel.
> Subagent-implemented (found+fixed a loose_eq bug: Symbol()==0 → false), then I reviewed + SHIP-review
> subagent (bounds/termination/additive all confirmed; sym_key buf ≤26/32; 8 enumeration sites hide
> @@sym) + jstest PASS (Symbol/custom-iterable/class-iterable/adversarial cases) + in-OS (js ITER.JS →
> "typeof symbol sum 10", a [Symbol.iterator]() range summed by for-of). KNOWN (pre-existing, unrelated):
> object literals as ternary branches `c?{..}:{..}` mis-parse; spread/Array.from don't invoke
> [Symbol.iterator] on plain objects (only for-of does). Baked ITER.JS demo. Files: kernel/js.c,
> tests/js/suite.js+.expected, tools/mkfatfs.c.
> M468: **Shell output redirection `>` / `>>`** — `cmd > file` (overwrite)
> + `cmd >> file` (append), composing with M463 pipes (`ls | grep TXT > found.txt`). Reuses the M463
> opt-in print-capture: main() parses+strips a trailing `>`/`>>`+filename, then the command (single OR
> a pipeline whose final stage is captured) runs under cap_begin/cap_end and the bytes go to the file
> via write_redirect (append reads existing first, both capped 8KB BSS). run_pipe gained (rfile,append)
> params for its final stage. Help line documents `|`/`>`/`>>`. Verified in-OS (>, >>, pipe+redirect,
> plain-pipe regression — all good) + all 7 suites + clean disk rebuild. SAFE/userspace; same write-path
> reliance as pipes. Files: user/shell.c only.
> M467: **Window close (F8 keyboard + mouse X) with app termination** —
> the DE gains a keyboard window-close (F8 → scancode 0x42 → WM code 0x1A), and closing an app window
> (F8 OR the title-bar ×) now TERMINATES the app instead of orphaning it (was: task kept running, no
> window, leaking slot+memory — so M464/M466 only helped self-`exit` apps). Cooperative kill: WM sets
> a->kill + task_wake; the app returns from its blocking app_sys_read, sees kill at the loop top, sets
> exited=1 + task_exit from its OWN context → M464/M466 reaper reclaims it. (Bug found+fixed mid-impl
> via temp kprintf: first cut called task_exit WITHOUT setting exited=1, so app_alive stayed true and
> the reaper skipped it — task dead but window+slot leaked. Set exited=1 first.) Verified: 10 spawn+F8
> cycles → free memory flat at the 102 MiB baseline, 0 faults, 0 orphans, all 7 suites. THE KEYSTONE:
> the teardown now applies to the normal close path. Files: app.c, app.h, keyboard.c, desktop.c.
> Remaining deferred: forced-kill of a TRULY-stuck app (one not reading input — rare), ~150-root TLS
> chain enforcement (bulky), FAT32 write-robustness (fragile).
> M466: **App address-space reclamation (full teardown)** — completes
> M464. New `vmm_destroy_address_space` (vmm.c) frees an exited app's page tables AND user frames
> (ELF+stack, ~59 frames/app), called from app_reap. PROVABLY SAFE: vmm_create copies boot's PDPT
> verbatim + next_table only writes not-present slots ⇒ a PDPT entry DIFFERING from boot's is always
> app-private; free exactly those (+ private PML4/PDPT pages), skip the shared low map + higher half.
> ROOT-CAUSE FIX in task_exit: it left the dead task's CR3 loaded under `next` (only swapped rsp), so
> the reaper saw the dying space active and the guard refused to free (diagnosed via temp kprintf:
> active==dead-cr3); task_exit now loads next->cr3 like switch_to_next, so the WM reaps in kernel_pml4
> and frees. Both SHIP-reviewed; verified in-OS: free memory returns to the EXACT boot baseline (102
> MiB) after 12 spawn+exit cycles (dropped to 96 before), 0 faults. The app-exit leak is fully closed.
> Remaining deferred: forced-kill of X-closed orphaned apps (task still running — risky), ~150-root TLS
> chain enforcement (bulky), FAT32 write-robustness (fragile).
> M465: **Kernel-heap interrupt-safety** — `kmalloc`/`kfree` (kheap.c)
> now bracket their shared-free-list edit with irq_save(cli)/irq_restore, closing the pre-existing
> latent race the M464 review surfaced (an IF-on WM `kfree` preempted by an app's IF-clear syscall
> `kmalloc` over a half-linked list). Restores the caller's prior IF state (safe on/off), nests across
> kmalloc's grow-and-retry recursion. Correctness-only (single-threaded behaviour unchanged). Verified:
> all 7 suites + in-OS stress (5 spawn+exit cycles freeing 256 KB stacks from the WM, interleaved with
> a heap-heavy CSS-page browser render — 0 faults). The prior review explicitly recommended this fix.
> M464: **App-exit resource reclamation** — a ring-3 app that exits
> cleanly now has its `task_t` + 256 KB kernel stack freed and its `apps[]` slot released (was: all
> leaked → 8-spawns/boot hard cap). New `task_free` (task.c) + `app_reap` (app.c); the WM reap loop
> (desktop.c) frees from its own task only once the dead app's task is `TASK_DEAD` — set interrupts-off
> and only left via the final context_switch, so observing it from another task proves off-CPU (no
> UAF). The WM drops the window only after the slot is reclaimed (no dropped-window leak). I designed
> it after reading vmm/task/app/desktop, then SHIP-reviewed (off-CPU invariant airtight) + all 7 suites
> + in-OS verified (10 spawn+exit shell cycles, 0 faults, 0 spawn failures — old code fails at #8).
> DEFERRED (documented, not session-length): freeing the app's address space (a->cr3 page tables +
> user frames — needs an active-CR3 guard) and forced termination of X-closed (orphaned, still-running)
> apps. NEXT surfaced by the review: the kernel heap (kmalloc/kfree) has NO locking — an IF-on desktop
> kfree can be preempted by an app's IF-clear syscall kmalloc over a half-edited free list (pre-existing
> latent; M464 widens it a hair). Wrapping kmalloc/kfree/kzalloc in irq_save/irq_restore would close it.
> M463: **Shell pipes (`cmd1 | cmd2`)** — the userspace shell now
> does UNIX-style N-stage pipelines (`ls | grep TXT`, `cat f | wc`). The command dispatch was
> extracted into `run_command(line,cwd)` (whole if/else chain wrapped in `do{…}while(0)` so its
> dispatch-level `continue`s keep meaning — a plain command runs byte-for-byte as before — and "exit"
> returns 1); `print()` in ulib got an opt-in capture mode (off for every other program); `run_pipe`
> captures each stage to PIPE.TMP that the next stage reads as its trailing file arg (so grep/wc/sort/
> head/… consume piped data unchanged). Implemented via a subagent (do-while(0) trick eliminated the
> continue-conversion risk), then I reviewed (run_command/run_pipe/ulib all sound) + all 7 suites +
> in-OS verified (ls|grep filters, echo|wc counts 1/3/12). Cosmetic: wc-style commands echo "PIPE.TMP"
> as the filename — inherent to the temp-file model. Userspace-isolated/recoverable.
> M462: **SVG `<use>`/`<symbol>` reuse** — `<use href="#id" x= y=>`
> (+ `xlink:href`) instantiates a defined element (shape, or `<symbol>`/`<g>`, usually in `<defs>`)
> at an offset. The render loop was extracted into `render_region(ctx,p,end,depth)` (behaviour-
> preserving at depth 0 — the 10 unit tests lock it byte-for-byte); `find_def` locates the span
> (depth-matching the close), `<use>` renders it under translate(x,y), recursion depth-capped (<4,
> self/cyclic terminate). Implemented via a subagent, then I reviewed (find_def/`<use>` bounds-safe)
> + svgtest PASS (11 unit tests + fuzz + 2M-iter focused) + in-OS verified (USE.SVG: a `<g id=star>`
> `<use>`d 4× → 4 stars). **SVG decoder now fully comprehensive.**
> M461: **CSS `text-decoration:line-through`** — `<s>`/`<del>` tags
> already struck (STY_STRIKE + strike-line); now CSS line-through (inline/`<style>`) maps to it too.
> One additive check in parse_style_textstyle. Verified in-OS (CSS.HTM struck span). 
> **NOTE/STATUS: I've now done 21 milestones this session (M441-461): SVG (rasterizer/transforms/
> inheritance/opacity/gradients/text), CSS (bg/align/font-size/display:none+visibility/comma-selectors/
> hsl/line-through), tables, blockquotes, TLS hostname+validity enforcement+cert-info, browser zoom.
> The safe-high-value space is SATURATED; remaining valuable work (shell pipes, SVG `<use>`, VMM
> app-exit teardown, ~150-root chain enforcement) all need RISKY REFACTORS of working code — deferred
> to protect working systems per the original directive. Continuing with safe genuine additions.**
> M460: **CSS `hsl()`/`hsla()` colours** — modern stylesheets use HSL;
> the parser only did hex/rgb/named. Added an integer HSL→RGB (`hsl_to_rgb`, no FPU) + an `hsl(...)`
> branch in parse_color, so HSL works for color/background/`<font>`/cascade. Additive. Verified in-OS
> (CSS.HTM: hsl red/green/blue text + hsl-yellow bg). (SVG fill=hsl() not added — separate svg.c parse_color.)
> M459: **comma-grouped CSS selectors** — `h1, h2, .title { }`-style
> rules (ubiquitous in real CSS) were dropped (sel_parse rejects the comma). capture_css now parses
> the decls once + adds one rule per comma sub-selector that parses. Additive (single selector = 1-elem
> list), bounded by CSS_MAX, no matching/render change. Verified in-OS (`.g1, .g2, em {…}` colours all
> three; others unregressed).
> M458: **CSS `visibility:hidden` + hidden `<img>`** — completes hiding
> (M454/455): `visibility:hidden` (inline/`<style>`) hides like display:none via the same scope (1 line
> in parse_style_display); a void `<img>` with its own display:none/visibility:hidden/hidden renders
> nothing (the deferred standalone-hidden-img case). Additive, no render-loop change. Verified in-OS.
> NOTE: standalone SVG browsing already works (try_image); SVG `<use>`/`<symbol>` deferred (needs a
> render-loop refactor — risky to the working decoder; worktree isolation unavailable here).
> M457: **browser content zoom** — `+`/`-`/`0` scale page text 1×–4×
> (persists across navigation). Multiplies the per-token `sc`/line-height the renderer already
> computes (headings/font-size/bold all scale) + the wrap reflows. Additive (zoom 1 = unchanged).
> Verified in-OS (text file ~3× + reflowed). Note: images don't zoom (text-only).
> M456: **SVG `<text>`** — svg.c renders `<text>` with the kernel 8×16
> bitmap font (`font_glyphs`, isolated; svgtest stubs it), each glyph scaled to `font-size` device px
> + `fill`-coloured, anchor through transform/viewBox map (translate/scale positioning; no glyph
> rotation). Bounded (blend_px clamps every px + char/height caps) → fuzz-safe. svgtest +text unit
> test +text fuzz frags. Verified in-OS (TXTSVG.SVG: "Hello, SVG!" 22px blue, "text labels work" 15px
> red, "OK" on a circle). The SVG decoder is now comprehensive: shapes/paths + transforms +
> inheritance + opacity + gradients + text.
> M455: **`display:none` + `font-size` from `<style>` rules** — M448/M454
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
