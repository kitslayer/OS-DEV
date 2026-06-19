# What's next

> **(M784) Shell help — document line editing.** Added an `edit:` line to `help` listing the
> line-editing keys (arrows, Home/End, Del, up/down history, Tab completion) and the M782 Ctrl kills
> (`^W`/`^U`/`^K`) + `^C` cancel — so the terminal's editing power is discoverable. Pure doc string; `make
> check` 30/30; verified in-guest.

> **(M783) Editor — Ctrl-S save / Ctrl-Q quit (now that Ctrl exists).** With M782's Ctrl support, the editor
> finally gets the save/quit options it lacked: Ctrl-S saves and keeps editing ("[saved]"), Ctrl-Q quits
> *without* saving (abandon changes) — ESC still saves & quits. Closes the real gap where ESC was the only
> exit (no way to save-and-continue or to discard a bad edit). Status hint updated to `ESC/^S=save ^Q=quit`.
> `make check` 30/30. Verified in-guest: Ctrl-S → "[saved]" + still editing (5b written); Ctrl-Q → editor
> closes, the later unsaved keystroke discarded.

> **(M782) Keyboard — Ctrl modifier + readline shortcuts in the terminal.** The OS had no Ctrl key at all
> (the cooked-keycode space was claimed by arrows + WM F-keys). Added `ctrl_down` tracking (LCtrl scancode
> 0x1D) and encode Ctrl+letter as `0x80|(letter&0x1f)` (0x81–0x9A) — a clean range that doesn't collide with
> the existing codes. The line editor (`app_sys_read`) maps them to readline shortcuts: Ctrl-A/E (home/end),
> Ctrl-B/F (left/right), Ctrl-P/N (history), Ctrl-D/H (delete/backspace) via remap to existing codes, plus
> new Ctrl-K (kill to end), Ctrl-U (kill line), Ctrl-W (kill word), Ctrl-C (abandon line, prints `^C`).
> Benefits every `readline` caller (shell, calc, adv); unblocks future Ctrl shortcuts in the editor. `make
> check` 30/30. Verified in-guest: Ctrl-W/U/K kills + Ctrl-C cancel all work.

> **(M781) Build — `-fwrapv` for the OS-authored userspace.** The user apps compiled *without* `-fwrapv`
> (only the kernel had it), so signed overflow was UB under `-O2` — the latent class M779's fuzz exposed in
> the shell evaluator, and which calc's parse/power/`*` paths share. Added `-fwrapv` to `USER_CFLAGS` so all
> OS-authored apps' signed arithmetic wraps (defined), same rationale as the kernel; ported games keep their
> own CFLAGS. All 231 user objects rebuilt (via M780's dep tracking), `make check` 30/30, calc verified
> in-guest (`2+3*4`=14, `0xff&0x0f`=15, `2^10`=1024).

> **(M780) Build — header-dependency tracking.** The Makefile didn't track header deps, so editing a header
> left objects stale — M779's `shmath.h` change needed a manual `touch shell.c` to take effect (a real
> ship-the-wrong-binary hazard). Added `-MMD -MP` to the kernel + user CFLAGS (gcc drops a `.d` beside each
> `.o`), `-include`d every `build/**.d` at the *end* of the Makefile (so the generated rules can't hijack the
> default goal), and made the `%.o` rules depend on the Makefile too (CFLAGS changes now rebuild). Verified:
> a clean build emits 117 `.d`; touching `shmath.h` rebuilds only `user_shell.o`, touching `app.h` rebuilds
> exactly the 4 kernel files that include it; `make check` 30/30.

> **(M779) Shell `$((expr))` — full bitwise/shift/power operators (bash-compatible).** Extended the shared
> evaluator (`user/shmath.h`) from `+ - * / %` to bash's whole `$(())` operator set + precedence: `**`
> (power, right-assoc), `* / %`, `+ -`, `<< >>`, `&`, `^` (XOR), `|`, and unary `~`. Hardened all arithmetic
> to be overflow-safe (unsigned-wrap, defined behaviour) after the host fuzz immediately caught **real
> signed-overflow UB** in the power loop — div/0 and `MIN/-1` and out-of-range shifts now yield 0/wrap with
> no UB. Extended `tests/shmath` (17 new asserts + fuzz alphabet `<>&^|~`); 300k iterations ASan/UBSan-clean.
> `make check` 30/30; verified in-guest: `$((1<<4))`→16, `$((0xff&0x0f))`→15, `$((2**8))`→256, `$((100>>2))`→25.

> **(M778) Editor — line-of-total in the status bar.** The status line now shows `L<current>/<total> C<col>`
> (total = logical line count) instead of bare `line:col`, so you can see your position in a long file —
> pairing with the M771 PgUp/PgDn paging. Safe display-only change (no effect on the wrap/scroll/cursor
> model; a per-line gutter would have needed reworking the grid-auto-wrap path, deferred). `make check` 30/30.
> Verified in-guest: README.TXT shows `L3/3 C1`.

> **(M777) Host-test the shell `$((expr))` evaluator.** Extracted the M762 arithmetic evaluator out of
> shell.c into a shared header `user/shmath.h` (the verbatim-extraction pattern, like cssprop/url/shgrep): a
> pure recursive-descent integer evaluator whose only shell dependency is the `sh_var()` variable-lookup
> hook the includer supplies. shell.c now `#include`s it and provides `sh_var` (via `vget`); behaviour is
> byte-identical. Added `tests/shmath` — 25 regression asserts (precedence, parens, hex, unary, /0, vars,
> associativity) + 300k random-expression fuzz iterations under ASan/UBSan — wired into `make check` as
> `shmathtest` (now 30 host suites). Verified in-guest unchanged: `$((2+3*4))`→14, `set N=6; $((N*N+1))`→37.

> **(M776) Paste-buffer race hardening.** `app_paste` now zeroes `paste_len` before refilling the per-app
> paste buffer, so if a second paste arrives while the first is still draining, `iq_get` can't read a
> half-overwritten buffer (it sees `paste_pos < paste_len == 0` → skips it until the refill sets the new
> length). Tiny correctness fix to the M768 buffer. `make check` 29/29; normal paste verified unchanged.

> **(M775) Browser paste — insert at the caret.** Consistency fix for M773/M774: middle-click paste now
> inserts the clipboard at the caret (`url_cur` for the address bar, `field_cur` for a focused field) instead
> of always appending — control chars skipped, caret left after the inserted text. `make check` 29/29.
> Verified in-guest: copy "from-scratch", type "X" in the address bar, Home, paste → "from-scratchX" (caret
> respected). Line editing + clipboard now behave consistently across terminal, address bar, and form fields.

> **(M774) Browser form fields — mid-line editing.** Completes "line editing everywhere" (terminal M757,
> address bar M773, now form inputs). The focused `<input>`'s `|` cursor now sits at `b->field_cur` within
> the value (not just the end): left/right move it, Home/End jump, Delete removes at it, Backspace before it,
> printable keys insert at it; the field render places `|` mid-value and the focus path sets the caret to the
> value's end. `make check` 29/29. Verified in-guest on `file:form.htm`: focus the name field, type "World",
> Home, type "Hello-" → `[Hello-|World]` (inserted at the start, caret mid-value).

> **(M773) Browser address bar — mid-line editing.** The URL bar was append/backspace-only (fixing a typo
> near the start meant retyping) — the same gap the terminal had before M757. Added a caret (`b->url_cur`):
> left/right move it, Home/End jump, Delete removes at the caret, Backspace before it, printable keys insert
> at it; the bar scrolls to keep the caret visible and draws it at its position. Entry points
> (`browser_click` on the bar, `browser_paste`) set the caret. `make check` 29/29. Verified in-guest: type
> `example.com`, Home, type `go-` → `go-example.com` (inserted at the start, caret mid-string). Form-field
> mid-line editing is still future (same pattern, per-field cursor).

> **(M772) Browser — double-click word-select.** Matches the terminal's M765: double-clicking a word in a
> page selects it (highlighted) and copies it to the clipboard. New `browser_sel_word(b,rx,ry,out,max)` (hit
> the word token, highlight it, extract). The WM detects a double-click in the browser body (reusing the
> `last_body_click` timer + proximity) before the scrollbar/link/drag paths. `make check` 29/29. Verified
> in-guest: double-click "from-scratch" on the start page → middle-click the address bar pastes it. Selection
> is now consistent across terminal + browser (drag-range + word + copy; paste everywhere).

> **(M771) Editor — PgUp/PgDn + wheel paging.** The editor couldn't page long files: PgUp/PgDn (0x15/0x16)
> were swallowed by the terminal-scrollback intercept before reaching it, and the wheel did nothing there.
> Now `app_key` only intercepts 0x15/0x16 for ordinary terminals; a full-screen self-drawing app (`caret_off`,
> e.g. the editor) receives them as keys. The editor pages by EDVIS-1 (15) lines on PgUp/PgDn; the wheel,
> routed via a new `app_caret_hidden()` accessor, sends arrow keys (3 lines/tick) to such apps and scrollback
> to terminals. `make check` 29/29. Verified in-guest: open a 60-line file → 2× PgUp jumps from line 61 to
> line 31.

> **(M770) Browser scrollbar — click + drag to scroll.** The symmetric counterpart to M769: the browser's
> right-edge scrollbar is now interactive. Click the track to jump, drag to scroll. New
> `browser_in_scrollbar(b,rx,ry,w,h)` (hit-test, only when content overflows) + `browser_scroll_track(b,ry,h)`
> (maps track-y → `b->scroll`, render clamps); the WM checks the scrollbar before the link/select path and
> drives a `bsbdrag` gesture. `make check` 29/29. Verified in-guest: click the bottom of the bar → page
> footer; drag up → page top. Scrolling is now consistent across terminal + browser: wheel, keyboard, and a
> click/drag scrollbar.

> **(M769) Terminal scrollbar — click + drag to scroll.** The M764 scrollbar was indicator-only but looks
> interactive. Now clicking anywhere on its track jumps the scrollback to that position, and dragging the bar
> scrolls continuously. New `app_scroll_frac(a, num, den)` maps a track fraction to `view` (top = oldest,
> bottom = live). The WM hit-tests the scrollbar column on left-press (a few px of slop for grabbing),
> starts an `sbdrag` gesture, and the grip/title-bar/selection paths are unaffected (grip is checked first;
> the bar sits just right of the text). `make check` 29/29. Verified in-guest: click the track top → jumps to
> the banner; drag down → back to the live prompt.

> **(M768) Long-paste correctness — paste buffer + bigger shell line.** Two caps truncated long pastes (e.g.
> a multi-line browser selection): `app_paste` injected via the 128-entry key queue (excess dropped), and the
> shell read into a 128-byte line. Fixed both: each app now has a `pastebuf[CLIP_MAX]` that `iq_get` drains
> *before* the key queue (so a paste up to 2 KB isn't capped by `IQ_SIZE`, and it's delivered to both
> `app_sys_read` and `sys_pollkey` callers — shell, editor, calc, adv); and the shell's input line grew
> 128 → 1024 (long URLs for `wget`, long pastes). `make check` 29/29. Verified in-guest: drag-select a big
> browser paragraph → paste into the shell → the full ~290-char text lands (previously cut at ~128).

> **(M767) Browser text selection — the clipboard capstone.** Drag across page text to select it (word
> granularity, highlighted white-on-blue) and copy to the shared clipboard on release. The render loop now
> records a per-visible-word rect (`b->wrec`, reusing `lrec_t` with `.link` = token index) for hit-testing,
> and highlights tokens in `[tsel0,tsel1]`. New `browser_sel_begin/extend/commit/clear` + `browser_hit_word`;
> `browser_sel_commit` joins selected words with spaces (newline at block breaks), trimming. Low-risk: a left
> press over a **link** still follows it (browser_click returns 1); selection starts only on non-link content
> (browser_click now returns 0 there — its return was previously ignored). Selection clears on reparse.
> `make check` 29/29 (golden-safe: highlight only when a selection exists). Verified in-guest: drag a line on
> the start page → words highlight → middle-click the address bar pastes the exact text. **The clipboard is
> now universal + bidirectional:** terminal↔terminal, terminal↔browser, browser text/links → anywhere.

> **(M766) F1 help — document the new mouse gestures.** The session added a lot of mouse UX (wheel scroll,
> terminal drag/word selection + middle-click paste, browser right-click link copy) that was undiscoverable.
> Extended the F1 keyboard-shortcuts overlay with a mouse section: "Wheel scrolls the window under the
> cursor", terminal select/copy/paste, and browser link-copy/paste. Pure UI text; the panel auto-sizes to
> the line count. `make check` 29/29. Verified in-guest: F1 shows the new lines, rendered cleanly.

> **(M765) Terminal — double-click word-select.** Completes the M760 selection UX (which was drag-only):
> double-clicking a cell selects the whitespace-delimited word under it and copies it to the clipboard — so
> a word or URL can be grabbed without precise dragging. New `app_sel_word(a,row,col)` (expands over
> non-spaces, then `app_sel_commit`); the WM detects a double-click in a text-app body via a short timer +
> proximity test (mirroring the titlebar's maximize double-click), else falls back to drag-select. `make
> check` 29/29. Verified in-guest: double-click `COPYPASTE` selects exactly the word (tight bounds) →
> middle-click pastes it. Terminal selection now = drag-range + word + middle-click paste.

> **(M764) Terminal — scrollback scrollbar.** The terminal had scrollback (PgUp/PgDn + the M759 wheel) but
> only a tiny `^` indicator. Added a real scrollbar on the right edge of the text grid: a dark track with a
> thumb sized to visible/total rows and positioned by `view`, so you can see how much history exists and
> where you are — parity with the browser, completing the terminal scrolling UX (line editor + scrollback +
> wheel + scrollbar). Only shown when `sb_count > 0` (the `^` is the fallback), so golden-safe. ~10 lines in
> `app_render`; `make check` 29/29. Verified in-guest: after `help`, the thumb sits at the bottom (live), and
> wheel-up moves it to the top (start of history).

> **(M763) Browser — right-click a link to copy its URL.** Closes the browser→clipboard direction (M760/M761
> covered terminal↔terminal and terminal→browser). Right-clicking a link copies its href to the shared
> clipboard (status: "link copied"), skipping internal pseudo-links (javascript:/submit:/event:). Low-risk:
> right-click was unused, so the delicate left-click-follows-link path is untouched; full arbitrary-text
> selection in pages (needs per-token hit-test rects) is still future. New `browser_rclick(b,rx,ry,out,max)`
> returns the href; the WM (`clip_set`) stores it. Added `rclick X Y` to osdrive. `make check` 29/29.
> Verified in-guest: right-click `https://example.com` on the start page → "link copied" → middle-click the
> address bar pastes it. The clipboard now flows terminal↔terminal, terminal→browser, and browser→anywhere.

> **(M762) Shell — arithmetic expansion `$((expr))`.** Builds on the M756 env vars to make the shell
> scriptable: `$((...))` evaluates an integer expression and substitutes the result, anywhere on a line
> (expanded alongside `$NAME`, before glob/pipe/redirect). A small recursive-descent evaluator (in
> shell.c) handles `+ - * / %`, unary minus, parentheses, decimal/`0x` literals, and variable names (bare
> or `$`-prefixed, via the same `vget`); integer-only like the rest of the OS, with `/0` and `%0` → 0.
> App-only; `make check` 29/29. Verified in-guest: `$((2+3))`→5, `$((10*4-2))`→38 (precedence),
> `$(((2+3)*4))`→20 (parens), `set X=7; $((X*X))`→49 (variables), `$((100/7))`→14 (integer division).

> **(M761) Clipboard paste into the browser (cross-app copy/paste).** Completes M760's clipboard across app
> types: middle-clicking a browser window pastes the clipboard into the focused `<input>` field, or (none
> focused) into the address bar — entering edit mode, replacing its contents on a fresh paste — and raises
> the window so Enter submits. New `browser_paste(b, s, n)` (drops control chars / newlines, single-line);
> the WM reads the clipboard (`clip_get`) and hands the text to it, so browser.c needs no app.h. `make check`
> 29/29. Verified in-guest: `echo example.com` → drag-select in the shell → middle-click the browser → the
> address bar reads "example.com", ready for Enter. Still future: selecting text *in* the browser (needs a
> per-token x array for hit-testing) to copy from pages.

> **(M760) System clipboard — terminal text selection + middle-click paste.** The OS had no clipboard and
> no way to select text. Now: left-drag in a terminal selects a linear, line-spanning range (highlighted
> white-on-blue, extracted from the scrollback/live grid exactly as drawn, trailing spaces trimmed per line)
> and copies it to a shared kernel clipboard on release; middle-click pastes the clipboard into the text app
> under the cursor (X11 primary-selection style — injected into its input queue, so a shell runs each pasted
> line). One buffer (`clip_set`/`clip_get` in app.c) shared by every window, so text carries between them.
> New app APIs: `app_sel_begin/extend/commit/clear`, `app_paste`; WM drives them from a `selecting` drag
> state + a middle-button handler; typing clears the highlight. Added `mclick X Y` to osdrive. `make check`
> 29/29. Verified in-guest: `echo COPYPASTE` → drag-select the output → middle-click at the prompt pastes
> "COPYPASTE". Future: browser text selection feeds the same clipboard.

> **(M759) Mouse wheel scrolling.** The pointer had no wheel at all. Now the wheel scrolls whichever window
> the cursor is over: the browser (via `browser_key` arrow-scroll) and any text app's scrollback (via
> `app_key` PgUp/PgDn). Two input paths feed one shared accumulator (`mouse_add_wheel`/`mouse_read_wheel`):
> the USB tablet's HID packet carries the wheel in byte 5 (what `make run`/osdrive actually use), and the
> PS/2 driver now does the IntelliMouse sample-rate knock (200/100/80 → device id 0x03 → 4-byte packets with
> a Z axis) for real-hardware/PS-2 configs. The compositor polls the accumulator each frame and routes ticks
> to the topmost non-minimized window under the cursor (gfx apps skipped). Added a `wheel X Y N` command to
> osdrive for testing. `make check` 29/29. Verified in-guest: wheel scrolls the browser both ways and the
> shell scrollback (`^` indicator).

> **(M758) Editor — Home/End/Delete/Tab + system-caret opt-out.** The full-screen editor only had arrow
> keys + backspace. Now it uses M757's new keycodes: Home/End jump to the start/end of the current line,
> Delete forward-deletes the char at the cursor, and Tab inserts spaces to the next 4-column stop. Also
> added a small `sys_caret(on)` syscall (SYS_caret 56) so a full-screen app that draws its own cursor can
> hide the system block caret — the editor calls `sys_caret(0)` after its filename prompt, so the prompt
> keeps the caret but the body shows only its own `|` (no double cursor). `make check` 29/29. Verified
> in-guest: Home+insert, Delete, End+append, Tab→4 spaces; and the editor body shows a single cursor.

> **(M757) Terminal — real line editor + visible caret.** The kernel's cooked-input read (`app_sys_read`)
> only ever appended/backspaced at the end of the line, and the terminal drew no caret at all. Now: a green
> block caret on the focused terminal at the live cursor (threaded `focused` through `draw_window` →
> `draw_content` → `app_render`); left/right arrows move the caret within the line; Home/End jump to the
> ends; Delete removes the char at the caret and Backspace the one before it — both work mid-line, repainting
> the tail; printable keys insert at the caret. New keycodes: Home→0x01, End→0x05, Delete→0x04 (keyboard.c).
> Command history deepened 6 → 32 entries. Benefits every `readline` caller (shell, calc, editor, adv).
> Kernel-side (keyboard.c / app.c / desktop.c / app.h); `make check` 29/29. Verified in-guest: typed `echo
> Hllo`, ←←← then `e` → `echo Hello` → "Hello"; `echo XOK` → del 'X' → "OK"; Home+Delete; up-arrow recall.

> **(M756) Shell — environment variables.** `set NAME=value` (or `export NAME=value`) stores a variable;
> `$NAME` / `${NAME}` expand in any command line (before glob/pipe/redirect); `unset NAME` removes one;
> `set`/`env` list them. They persist for the shell process's lifetime. A real scripting power-feature the
> shell lacked, and a distinct subsystem from the browser run. App-only (`user/shell.c`, not test-coupled);
> `make check` 29/29. Verified in-guest: `set GREET=hello world; echo $GREET` → "hello world"; `set
> DIR=README.TXT; cat $DIR` opens the file; `env` lists both.

> **(M755) Browser — full-width block backgrounds.** `background-color` on a block element (div / p / h1-6 /
> section / li / …) now fills the whole line band (cl→cr, respecting any indent), not just behind the text —
> distinct from an inline element's bg (`<mark>`, `<span>`), which stays a text highlight. Block-ness is
> decided by the element's tag (`is_block_tag`) and marked with a spare bg flag bit (`0x02000000`); the render
> paints the band at each line start (the `cx==cl` point, after `ls`/indent is known). Real sites' coloured
> cards/sections/headers now render as boxes. `make check` 29/29 (the browser goldens are parser/presence
> checks, unaffected); verified in-guest — `div{background:#cfe8ff}` shows a full-width band, an inline span
> bg stays behind its text.

> **(M754) Desktop — click a file in the Files window to open it.** The Files window was keyboard-only
> (up/down + Enter); a mouse click on a file row did nothing (the click handler routed body clicks to the
> browser but had no Files case). Added `files_click()`: a click maps the cursor's y to the file row
> (mirroring the render layout + scroll offset), selects it, and opens it through the shared Enter path
> (`file:NAME` in a browser window). A distinct, non-browser UX fix. `make check` 29/29; verified in-guest —
> clicking `MARGIN.HTM` highlights it and opens it in a browser.

> **(M753) Browser — `margin`/`padding` shorthand horizontal value.** The shorthand only contributed its top
> value (vertical) before; `parse_style_hspace` now decodes the shorthand's left value too (4 tokens → 4th,
> 2–3 → 2nd, 1 → all sides), so the ubiquitous `margin: V H` form (and `margin: Npx` all-sides) indents the
> block. Parse-only, golden-safe. `MARGIN.HTM` gains a `margin:6px 60px` example. `make check` 29/29; verified
> in-guest (a `margin:40px` div is now indented, and `margin:6px 60px` indents 60px). The browser's CSS
> box-model spacing — margin + padding, both axes, inline + `<style>` rules + shorthands — is comprehensive.

> **(M752) Browser — CSS horizontal indent from `<style>` rules.** Mirroring M749, `capture_css`/`css_match`
> now carry a per-rule left indent (`css_indent[]`), so `margin-left`/`padding-left` from a stylesheet class
> (e.g. `.ind{margin-left:40px}`) indents the matched blocks — inline `style=` still overriding. With
> M748–M751 the browser's CSS box-model **spacing is now complete**: margin + padding, both axes, from inline
> styles and `<style>` rules. `MARGIN.HTM` gains a `.ind` example. `make check` 29/29; verified in-guest
> (a `<p class="ind">` is indented by the stylesheet rule).

> **(M751) Browser — CSS horizontal indent (`margin-left` / `padding-left`).** Completes box-model spacing in
> both axes: a block's left margin/padding now indents its whole content (wrapped lines included), reusing the
> `curindent`/`tokindent` machinery `<blockquote>` uses — saved/restored on the style-scope stack so nested
> indents compose. Inline `style=` for now. Golden-safe (no margin-left in the test pages). `make check`
> 29/29; verified in-guest — `<p style="margin-left:48px">` shifts the whole paragraph right, wrapped lines
> too. With M748–M750 the browser now does CSS box-model spacing (margin + padding) in both axes.

> **(M750) Browser — CSS vertical padding (rounds out the box-model spacing).** Extended the M748/M749 path:
> a block's top space now sums `margin-top` + `padding-top` (or the `margin`/`padding` shorthands' top value),
> so the very common `padding`-spaced containers render with their intended room. Renamed
> `parse_style_margin_v` → `parse_style_vspace`; same inline + `<style>`-rule cascade and golden-safety.
> `MARGIN.HTM` gains a padding example. `make check` 29/29; verified in-guest (`padding-top:45px` shows the
> gap). Browser vertical box-model spacing now covers margin + padding, inline and from stylesheets.

> **(M749) Browser — CSS margins from `<style>` rules (not just inline).** Extended M748: `capture_css` +
> `css_match` now carry a per-rule vertical margin (`css_margin[]`), so `margin-top` / the `margin` shorthand
> from a `<style>` block (tag / `.class` / `#id` / `[attr]` selectors) spaces block elements — with an inline
> `style=` margin still overriding the rule (correct cascade). This is the form real sites actually use.
> `MARGIN.HTM` extended with a `.gap{margin-top:55px}` rule. Golden-safe (the css test pages set no margins).
> `make check` 29/29; verified in-guest — a `<p class="gap">` shows the stylesheet-driven gap above it.

> **(M748) Browser — CSS vertical margins (first box-model spacing).** The renderer was explicitly
> "box-model-less"; now block elements honour `margin-top` and the `margin` shorthand's top value (px/em,
> capped at 120). Parsed from inline `style=` into a per-tag `pending_vmargin` that `emit_break` folds onto
> the block break's spacing field (`tok.off`), which the layout adds to the vertical advance — and since
> content height derives from that same cursor, scrolling tracks it automatically. Golden-safe: the test
> pages use no margins, so `off` stays 0 and rendering is byte-identical (csstest/browsertest unchanged). Key
> fix: consecutive block breaks (`</p><p>`) merge, so the margin had to be carried onto the *merged* break,
> not just a fresh one. New `MARGIN.HTM` demo. `make check` 29/29; verified in-guest — `margin-top:60px`
> shows a big gap, `:10px` a small one, `div margin:40px` spaces it. (CSS-rule margins via `<style>` next.)

> **(M747) JS engine — `globalThis` / `self`.** Modern JS and UMD-style libraries lean on the universal
> global object (`typeof globalThis`, `globalThis.X`, `self.X`); both were `undefined`, so that code broke.
> Built a global object at the end of `install_globals` that exposes every global defined above as a property
> (Math, JSON, Object, Array, String, the timer functions, console, document, fetch, window/location when in
> a page, …), with `self === globalThis` and `globalThis.globalThis === globalThis`. window/location are
> browser-only so they're skipped at the shell. Verified host (`globalThis.JSON.stringify`,
> `globalThis.setTimeout`, UMD detection) + suite golden updated; `make check` 29/29 green.

> **(M746) JS engine — `requestAnimationFrame`/`queueMicrotask`/`cancelAnimationFrame`.** Real pages drive
> animation with `requestAnimationFrame` and defer work with `queueMicrotask`; both were `undefined`. Wired
> them to the M745 deferred-callback queue (rAF/queueMicrotask enqueue like a 0-delay timer, so they fire
> first; cancelAnimationFrame cancels by id). `tests/js/timers.js` extended to cover them. 29/29 green.

> **(M745) JS engine — `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval`.** The engine had no timers
> at all (`typeof setTimeout` was `undefined`), so any real-world page that defers work with
> `setTimeout(fn, …)` — extremely common — aborted on "not defined". Added a deferred-callback queue in
> `kernel/js.c`: timers are queued during the run, then (the engine has no event loop — its I/O is blocking
> and Promises resolve eagerly) drained after the top-level script in (delay, registration) order, with
> callbacks free to queue more, bounded so a self-rescheduling timer can't spin forever. `setInterval` fires
> once (a synchronous engine can't loop a wall clock); `clearTimeout`/`clearInterval` cancel by id. New
> `tests/js/timers.js` golden (ordering + chaining + cancellation), wired into `make check`. 29/29 suites
> green; verified in-guest (`js` runs a `setTimeout` callback). Not games — a real browser/JS capability.

> **(M744) Pac-Man — persistent high score, wired into `scores`.** Pac-Man now saves your best to PACMAN.HI
> (like Snake/Tetris/…) and shows it in the HUD, and it's been added to the shell `scores` leaderboard. App-
> only change (pacman.c + shell.c, neither test-coupled). Verified in-guest: with PACMAN.HI=1234 on disk,
> Pac-Man's HUD loads **best 1234** and `scores` lists it.

> **(M743) New shell command — `scores` (a personal leaderboard).** Fifteen games save a best to their own
> `*.HI` file; `scores` reads them all and prints the ones you've set (skipping the rest), so you can see
> every personal best at once. App-only change to `user/shell.c` (not test-coupled). Verified in-guest: on a
> fresh disk it prints "(none yet)"; after `write`-ing a few `.HI` files it lists e.g. Snake: 4242, Tetris:
> 18800, Video Poker: 315.

> **(M742) New game — Pac-Man (maze chase).** `user/pacman.c`: eat every dot while dodging four ghosts; grab
> a power pellet (O) and they turn blue for a few seconds — touch them then and they flee to the pen for 200
> points. Clear the maze to win; lose three lives and it's over. Continuous movement with turn-queuing (your
> steer applies as soon as the way is clear); ghosts chase greedily (flee when frightened) at ~75% of your
> speed so you can escape. The maze uses regular wall pillars so every corridor is connected (no dot can be
> stranded). 29/29 suites green. Verified in-guest: the maze/dots/pellets render, Pac eats dots (+score),
> ghosts chase, and a catch costs a life and resets to start. Pac-Man and Chess were the two iconic games the
> set was missing — both now in.

> **(M741) Checkers — highlights the computer's last move (incl. multi-jumps).** Mirroring chess M740,
> `user/checkers.c` tracks the CPU's move — the first from-square and the final landing square of its slide or
> jump chain — and renders both in a distinct colour, so you can follow what it did (multi-jump captures were
> especially hard to read mid-board). App-only change (not test-coupled; last full `make check` at M739
> green). Verified in-guest: with the cursor moved aside, the CPU's reply shows its origin and destination
> highlighted, isolated from the cursor.

> **(M740) Chess — highlights the computer's last move.** After the CPU replied it was hard to see what it
> played (you had to diff the board). Now `user/chess.c` tracks the last move's from/to squares and renders
> them in bright cyan — the vacated square as a `*`, the moved piece in cyan — for both your move and the
> CPU's, so the post-reply board points right at the CPU's move. App-only change (chess.c isn't test-coupled;
> last full `make check` at M739 green). Verified in-guest: after 1.e4 the CPU's 1...Nf6 shows a cyan `*` on
> g8 and a cyan knight on f6.

> **(M739) New game — Missile Command (mouse-driven, in the framebuffer).** `user/missile.c`: defend six
> cities from descending missiles — move the mouse to aim the crosshair and click to detonate an interceptor,
> whose expanding blast destroys any missile it touches. A missile that reaches the ground takes out the
> nearest city; when all six are gone the sky turns red (r restarts, Esc quits), and the waves speed up. A
> graphics + **mouse** app — the first arcade game driven by the mouse — so it has its own SSE build rule.
> 29/29 suites green. Verified in-guest: the cities/battery/descending-missiles render, the crosshair tracks
> the mouse, and a held click detonates a (lingering) blast. (osdrive's instant `click` is too brief for the
> once-per-frame `sys_mouse` poll — the mouse analogue of the keyboard same-frame-tap — but a real press
> registers; `drag` confirms it.)

> **(M738) New game — Dots and Boxes vs the computer.** `user/dotsbox.c`: a 4×4-box grid (16 boxes on a 9×9
> dot/edge lattice). Move the `+` cursor with the arrows and Space to draw the edge under it; complete a box's
> fourth side to claim it (Y) and move again. The CPU completes any box it can (chaining its extra turns),
> else plays a "safe" edge that doesn't hand you a third side, else gives up the least it must. Pure-additive
> ulib app (last full `make check` at M737 green; kernel boots+runs it). Verified in-guest: edges draw, the
> CPU replies, and box completion + scoring + extra-turn chaining all work (it ran a chain to 7 boxes).

> **(M737) New game — Mancala (Kalah) vs the computer.** `user/mancala.c`: the classic sow-and-capture
> game. Pick a pit (1-6), seeds sow counterclockwise skipping the opponent's store; landing in your own store
> earns another turn, and landing the last seed in an empty pit on your side captures it plus the pit
> opposite. When a side empties, the other sweeps its leftovers — most seeds wins. The CPU plays a greedy
> heuristic (prefers extra-turn moves, then captures, then seeds banked, chaining its extra turns). 29/29
> suites green. Verified in-guest, including **seed conservation** (always 48 on the board) after a full
> exchange — strong evidence the sow/capture math is right.

> **(M736) New game — Video Poker (Jacks or Better).** `user/vpoker.c`: you're dealt five cards, toggle holds
> with 1-5, then Space draws replacements; the final hand pays on the classic 9/6 Jacks-or-Better table (a
> pair of jacks or better, up to a 250× royal flush). Each deal antes one credit; you start with 100 and your
> best balance persists to VPOKER.HI (a free 100 if you bust). Fills the casino-card gap (only Blackjack
> existed). Pure-additive ulib app; the last full `make check` (M735) was green and the kernel boots+runs it.
> Verified in-guest: the deal renders five suited, colour-coded cards, holds keep the right cards across a
> draw, and the hand evaluates correctly ("No win" on a junk hand).

> **(M735) Chess — castling.** Added castling to `user/chess.c`: rights are tracked in a bitmask (cleared
> whenever a king or rook leaves, or is captured on, its home square), the king may not castle out of,
> through, or into check, and `apply()`/`undo()` now snapshot the rights alongside the 64-byte board so the
> alpha-beta search treats castling correctly too. It's encoded as the king's two-file step — `apply()` spots
> that and moves the rook to match (h→f kingside, a→d queenside). App-only change. Verified in-guest: after
> e4/Nf3/Bc4, **O-O** moves the king to g1 and the rook to f1, and the CPU keeps playing legally. (En-passant
> is still not modelled — rare.)

> **(M734) Docs — README front page caught up.** The headline still said "720+ milestones / forty-plus apps /
> ~thirty games" and the M706+ recap pre-dated the recent work. Bumped to 730+ milestones / fifty-plus apps /
> ~forty games and added the additions since: the **Game Boy emulator** (Peanut-GB), **Chess** (alpha-beta
> AI), Checkers, Gomoku, Frogger, Lunar Lander, Yahtzee, Flappy, and the on-disk ELF loader (`run NAME.ELF`).

> **(M733) Chess AI plays sensibly now — piece-square tables.** The depth-3 search was material-only, so it
> answered 1.e4 with the passive 1...Na6 — any equal-material move scored the same. Added Michniewski's
> simplified piece-square tables to `evaluate()` (centre control, development, advanced pawns, a tucked-away
> king), small enough never to outweigh a real capture but enough to break ties toward good squares. App-only
> change (no kernel/test-coupled code). Verified in-guest: the CPU now answers 1.e4 with the developing
> 1...Nf6 instead of 1...Na6.

> **(M732) New game — Chess vs the computer (full legal-move rules + an alpha-beta AI).** `user/chess.c`:
> every piece moves by the real rules, you may not leave your own king in check, pawns auto-promote to a queen
> on the last rank, and the game ends on checkmate or stalemate. You play White (bottom); the CPU plays Black
> with a depth-3 negamax + alpha-beta search over material. Arrows move the cursor, Space picks one of your
> pieces then its destination (Space elsewhere re-picks), r starts a new game, q quits. (Castling and
> en-passant are deliberately not modelled.) The biggest gap in the board-game set — TTT, Connect-4, Reversi,
> Checkers, Gomoku and Battleship existed, but no Chess. Verified in-guest: the board renders in the start
> position, **e2-e4** plays, and the CPU answers with a legal developing move (Nb8-a6). 29/29 suites green.

> **(M731) Docs/correctness — the disk ELF loader is real (the embed comment was stale).** `kernel/app.c`
> already has `app_spawn_from_file()`, wired in `kernel/syscall.c` so `run NAME.ELF` falls back to loading an
> ELF straight from the FAT32 disk when the name isn't a built-in; `mkfatfs` even ships `CALC.ELF` for it and
> `GUIDE.TXT` documents `run NAME.ELF`. But `user_blob.asm` still claimed *"we have no general program loader
> from disk yet"* — corrected that comment to describe reality (the embedded apps need no disk; the kernel
> also loads ELFs from disk at runtime). Verified in-guest: `run calc.elf` opens a window titled **calc.elf**
> (loaded from disk via the path-as-title path, not the embedded "Calc"), which also confirms the M720/M722
> `fat32_read` perf rewrite reads binary files correctly.

> **(M730) New game — Frogger (cross the traffic).** `user/frogger.c`: hop your frog upward across lanes of
> cars, each lane scrolling at its own speed and direction, to reach the goal bank at the top (+10, then start
> again from the bottom). Touch a car and you lose a life; three lives, best saved to FROGGER.HI. Real-time —
> the lanes scroll on a tick, you hop on a keypress. Verified in-guest (lanes render with traffic, the frog
> hops, a collision squashes it and costs a life). 29/29 suites green.

> **(M729) New game — Gomoku (Five in a Row) vs the computer.** `user/gomoku.c`: place stones on an 11×11
> board, first to five in a row in any direction wins; you're X, the CPU is O. The CPU plays a heuristic — for
> every empty point it weighs the line it would build for itself against the line it would deny you
> (run-length²), slightly favouring blocks and the centre, so it both attacks and defends. Additive ulib app
> (kernel rebuilds clean; no test-coupled code touched). Verified in-guest: the board renders, you place X,
> and the CPU answers with O near your stones.

> **(M728) New game — Checkers vs the computer.** `user/checkers.c`: standard American draughts on an 8×8
> board — men move/capture one diagonal forward, kings both ways, capturing is **mandatory** and multi-jumps
> must be continued, and a man reaching the far row is crowned. Pick a piece (Space) then a destination (legal
> targets marked `*`); the CPU plays greedily (captures when able, continuing multi-jumps). Take all the CPU's
> pieces or leave it with no move to win. Verified in-guest: the board renders, selecting a piece shows its
> legal moves, a slide executes, the CPU replies, and the mandatory-jump rule is enforced ("you MUST jump").
> All 29 `make check` suites green.

> **(M727) New game — Yahtzee (with a persistent best).** `user/yahtzee.c`: solo Yahtzee — each of 13 turns,
> roll the five dice, hold any (1-5) and re-roll up to twice (r), then assign them to one of the 13 scoring
> categories (arrows to an empty row, Enter to score); the scorecard shows each empty row's potential score
> live. Standard scoring including the upper-section 63+ bonus (+35); best total saved to `YAHTZEE.HI`.
> Verified in-guest (dice + live scorecard render; scoring a category fills it and starts the next turn).
> All 29 `make check` suites green.

> **(M726) New game — Lunar Lander (vector, in the framebuffer).** `user/lander.c`: ride gravity down and set
> the lander on the green pad gently — the main engine (Up) burns fuel and pushes up, side thrusters
> (Left/Right) nudge horizontally; land on the pad slow and near-vertical to win, else crash. A starfield +
> jagged terrain with a highlighted pad, a fuel gauge, and the lander tinted white/amber by descent speed
> (green/red on land/crash). Built with SSE for the float physics; raw make/break input with the same
> same-frame-tap latch as the other graphics apps. Verified in-guest (the lander descends over the terrain;
> thrust + fuel work). All 29 `make check` suites green.

> **(M725) Game Boy emulator — a whole second console, via vendored Peanut-GB.** Same recipe as the NES:
> `user/gb/` vendors **Peanut-GB** (an MIT single-header DMG emulator — CPU/PPU/MBC) behind `gb_osdev.c`, which
> reads the chosen `.gb` off the disk, serves it through Peanut-GB's ROM/cart-RAM callbacks, maps each 2-bit
> LCD shade to the classic DMG green into a 160×144 framebuffer (`sys_gfx_blit`), and drives the joypad byte
> (active-low) from raw scancodes with the same same-frame-tap latch as the NES. A `.gb` picker handles
> multiple ROMs. Ships **Libbet and the Magic Floor** (`tools/libbet.gb`, Zlib, by Damian Yerrick) — a real GB
> puzzle game. Verified in-guest: libbet's intro renders in DMG green and input advances into the playfield
> (the "Combo / 0% / 0/04" HUD). All 29 `make check` suites green.

> **(M724) New game — Flappy Bird (with a persistent high score).** `user/flappy.c`: tap Space to flap the
> bird up against gravity and thread the gaps in the scrolling pipes; one point per pipe cleared, best saved
> to `FLAPPY.HI` (shown as `hi N`). Integer "sub-row" physics (no floating point, so it uses the generic user
> build rule), a 12-row field that fits the text grid, real-time pacing. Wired into the app registry + Apps
> menu. Verified in-guest (the `score/hi` header, bird, and start prompt render; flapping lifts the bird and
> pipes scroll in). All 29 `make check` suites green.

> **(M723) Persistent high scores for the new games (+ a Space Invaders layout fix).** Matching the OS's
> existing `*.HI` convention (SNAKE.HI / TETRIS.HI): **Space Invaders** now keeps a high score (`SPACEINV.HI`,
> shown as `hi N`, saved when beaten) and **15-Puzzle** keeps a fewest-moves best (`FIFTEEN.HI`, shown as
> `best=N`, saved on a faster solve). Adding the Space Invaders `hi` field surfaced a pre-existing bug — its
> header had been scrolling off the top of the 17-row text grid since M716 (the field was too tall) — so the
> field was shrunk and the full `score / hi / lives` header now shows. Verified in-guest (both display and
> play; persistence reuses snake's proven load/save). Isolated to two user-space games (no kernel or
> test-coupled code touched).

> **(M722) Faster file writes — `alloc_cluster` uses a free-cluster hint instead of rescanning the FAT.**
> Saving a file allocated clusters one at a time, and each `alloc_cluster` rescanned the FAT from the start
> for the first free entry — O(n²) for an n-cluster file (a 576 KB screenshot = 1152 clusters rescanned the
> whole FAT ~1152×). Allocation now starts each scan at a remembered hint (where the last one ended, with
> wraparound) and caches the FAT sector across the scan: a sequential write is O(n), and the clusters come out
> **contiguous** — which the run-coalescing reader (M720) then loads fast. It still returns only a genuinely
> free (entry==0) cluster, so a stale hint can at worst cause a false "disk full", never a clobber, and
> `free_chain` lowers the hint so freed space is reused. Verified: in-guest a 4 MB `cp` round-trips
> byte-identical (`cmp` → "files are identical"), and all 29 `make check` suites stay green (incl. fstest's
> 8k-op write/delete/mkdir fuzz and the exact-reclaim `df` regression).

> **(M720) Much faster file reads — `fat32_read` does multi-sector, contiguous-run I/O.** Loading a big file
> (e.g. Freedoom's 27 MB WAD) was reading the disk one 512-byte sector at a time *and* re-reading a FAT sector
> for every cluster (the same FAT sector up to 128× on a sequential walk) — ~108k single-sector PIO commands
> for 27 MB (~10-15 s). `fat32_read` now (a) caches the current FAT sector for the chain walk (local to the
> call, so no staleness vs. writes), (b) coalesces physically-contiguous, chain-linked clusters into runs, and
> (c) reads each run in ≤255-sector `ata_read` bursts straight into the destination (a bounce buffer only for
> the final partial sector) — a few hundred commands instead of ~108k. The cycle guard + `cluster_in_range`
> checks are preserved, and `remaining` is clamped to the caller's `max` so the destination can't overrun.
> This speeds up every file read (boot assets, DOOM/Quake/Freedoom loads, the browser's local pages). Verified
> in-guest: the Freedoom title now renders by ~6 s (was still a black screen at 7 s), and all 29 `make check`
> suites stay green — including `fstest`, whose corruption-fuzzer drives this exact read path.

> **(M719) Asteroids — a vector arcade game in the framebuffer.** `user/asteroids.c`: rotate/thrust a ship
> with wrap-around momentum physics, fire bullets, and blast asteroids that split (big → two medium → two
> small → gone); clearing the field spawns a bigger wave; three lives, a collision costs one. Drawn with a
> Bresenham line routine (ship triangle, octagon rocks, dot bullets) over a starfield; built with SSE for the
> float physics + a Taylor sin/cos, raw input with the same-frame-tap latch. Verified in-guest: ship + rocks
> + lives render, and rotating/thrusting/firing turns the ship and launches bullets while the rocks drift.
> All 29 `make check` suites still green.

> **(M718) Freedoom — a complete libre game on the DOOM engine, via a WAD picker.** The vendored engine
> already recognises Freedoom (`d_iwad.c` lists `freedoom1.wad`; `d_main.c` keys off the `FREEDOOM` lump), so
> this ships **Freedoom Phase 1** (GPL/BSD, `tools/freedoom1.wad`, ~27 MB — it fits the existing 64 MB disk)
> and gives the DOOM app a WAD picker (`user/doom/doomgeneric_osdev.c`, mirroring the NES ROM picker): it
> enumerates the `.wad` files on disk (`sys_list`) and, if there's more than one, shows a pickable text menu
> before starting — so DOOM now plays the shareware IWAD *or* the full free game. Verified in-guest: the
> picker lists DOOM1.WAD + FREEDOM1.WAD, and selecting Freedoom loads it (the 27 MB read takes ~10-15 s off
> the emulated IDE disk) and renders the Freedoom title + main menu. All 29 `make check` suites still green.

> **(M717) Apps menu: 2 → 3 columns.** This session's ~13 new game apps had filled the two-column Apps menu
> to the screen's bottom edge. The menu layout is fully parameterized, so bumping `MENU_COLS` to 3 (a one-line
> change in `desktop.c`) reflows the render, keyboard navigation, and click hit-testing together: all 48
> entries now show in three tidy 16-item columns (shorter than before, with headroom for ~36 more). Verified
> in-guest — the menu renders in three columns and arrow-nav + Enter still launch the right app (Down → Right
> → Blackjack). All 29 `make check` suites still green.

> **(M716) Two more games — Tron and Space Invaders.** **Tron light cycles** (`user/tron.c`): you vs a CPU
> that steers greedily (straight while safe, else turn to an open side); both leave solid trails on a 40×15
> arena, run into any trail or wall and you're out, last one riding wins; real-time. **Space Invaders**
> (`user/spaceinv.c`): a 3×8 invader block marches and drops + reverses at the edges (speeding up as you thin
> it) and occasionally shoots back; slide the cannon and clear them before they land — three lives. Both
> verified in-guest (Tron: trails + a dodged head-on; Invaders: formation + cannon). All 29 `make check`
> suites still green.

> **(M715) NES sound — APU wired to the streaming PCM ring.** The NES app's audio callback
> (`user/nes/nes_osdev.c`) now converts libxnes's per-sample float (requested at 48 kHz, the kernel's PCM
> rate) to 16-bit stereo and flushes the batch to `sys_pcm_stream` once per frame (~800 samples/frame,
> consumed at 48 kHz) — the same non-blocking streaming path DOOM/Quake use. Verified headlessly for
> stability: the emulator still loads a ROM, renders, and responds (Start → Main Menu) with the audio path
> active, no crash/hang/perf regression over several seconds; audibility itself needs a host audio backend
> (`make run`, not the headless `-audiodev none`). All 29 `make check` suites still green.

> **(M714) The raycaster is now a first-person SHOOTER.** Extends `user/raycast.c` with billboarded enemy
> orbs and combat: each column's wall distance is kept in a depth buffer, and living enemies are transformed
> into camera space, projected to a screen column + size (1/depth), and drawn as shaded ellipses only where a
> nearer wall doesn't occlude them (drawn far-to-near). A white crosshair marks the centre; Space fires at the
> nearest enemy under it that isn't behind a wall (short cooldown). Enemies slowly home in (a touch beeps +
> respawns them). Verified in-guest: an enemy orb renders down the corridor under the crosshair, and firing
> destroys it. All 29 `make check` suites still green.

> **(M713) A from-scratch raycaster — pseudo-3D in the framebuffer.** `user/raycast.c` renders a
> Wolfenstein-style first-person maze: one ray per screen column is marched across a 16×16 grid (DDA) to the
> first wall and drawn as a vertical slice whose height is 1/distance, coloured by cell and shaded by distance
> + which side was hit, over a sky/floor split. Arrows or WASD walk and turn (with wall collision); reach the
> green exit to escape. Built with SSE (its own Makefile rule, like DOOM/Quake) so the ray geometry can use
> float + a small Taylor `sin`/`cos` (no libm); input drains the raw make/break queue with the same
> same-frame-tap latch as the NES so both quick taps and held keys move. Verified in-guest: the 3D corridor
> renders, walking advances the view, turning rotates it. All 29 `make check` suites still green.

> **(M712) Two more games — Battleship and Pig.** **Battleship** (`user/battleship.c`): two 8×8 seas, both
> fleets (5/4/3/3/2) auto-placed at random; you fire at the hidden enemy sea (a crosshair you move) while a
> hunt-then-target CPU (random shots until a hit, then it works the neighbouring cells) fires back at yours —
> sink all 17 enemy cells to win. **Pig** (`user/pig.c`): the push-your-luck dice game — roll to build a turn
> total, but a 1 wipes it and ends your turn; hold to bank it; first to 100, and the CPU holds once it has 20.
> Both verified in-guest (Battleship: shots + AI reply scored on both seas; Pig: rolls accumulate the turn
> total). All 29 `make check` suites still green.

> **(M711) NES is now a multi-game console — a ROM picker.** The NES app (`user/nes/nes_osdev.c`) enumerates
> the `.nes` files on the FAT disk (`sys_list`) and, if there's more than one, shows a pickable text menu
> (up/down + Enter) before switching to graphics — so any `.nes` dropped on the disk just appears, no rebuild.
> Ships a second freely-licensed ROM beside Nova the Squirrel: the **240p Test Suite** (GPLv3, by Damian
> Yerrick / Artemio Urbina — `tools/240p.nes`, mapper 2 / UNROM). Verified in-guest: the picker lists
> NOVA.NES + 240P.NES, and selecting 240P renders its graphically rich main screen — exercising a second
> mapper through the same core. All 29 `make check` suites still green.

> **(M710) Two more games — Memory and Sokoban.** **Memory/Concentration** (`user/memory.c`): a 4×4 board of
> 8 letter pairs, Fisher-Yates shuffled; flip two cards a turn, matches stay face-up, tries counted, clear to
> win. **Sokoban** (`user/sokoban.c`): push every crate onto a goal (no pulling, no double-pushes); 3 bundled
> open-room levels (no corner traps, each hand-verified solvable), push counter, r restart / n next. Both
> additive ulib apps wired into the registry + Apps menu; verified in-guest (Sokoban level 1 solved on
> camera; Memory card flip). All 29 `make check` suites still green.

> **(M709) "Half-Life: Black Mesa" — a tribute survival shooter.** The real Half-Life can't run here (a
> closed GoldSrc/Win32 engine needing a GPU and proprietary assets; Quake is this OS's GoldSrc-family
> ceiling), so this is an honest original homage on the text grid (`user/halflife.c`): after the resonance
> cascade, a HEV-suited scientist survives a swarm of headcrabs that spawn from the edges and chase — arrows
> move/aim, Space fires a bullet in your facing direction, contact drains the HEV suit, and the spawn rate +
> enemy speed ramp with your kill score. Adds a "Half-Life" entry to the Apps menu. Verified in-guest (HUD +
> HEV bar, player, headcrabs chasing, firing). All 29 `make check` suites still green.

> **(M708) Three more games — 15-Puzzle, Mastermind, Pong.** **15-Puzzle** (`user/fifteen.c`): 4×4 sliding
> tiles shuffled by random legal slides (always solvable); arrows slide a tile into the gap; move counter.
> **Mastermind** (`user/mastermind.c`): crack a 4-peg / 6-colour code in 10 guesses with the standard
> black (`#` = right colour & spot) / white (`o` = right colour, wrong spot) two-pass scoring; type 1-6,
> Backspace erases, Enter submits. **Pong** (`user/pong.c`): real-time against a ball-tracking CPU paddle
> (Up/Down or W/S), wall + paddle bounces, first to 7. All additive ulib apps wired into the registry +
> Apps menu; each verified in-guest. All 29 `make check` suites still green.

> **(M707) Two new games — Reversi and Lights Out.** Additive ring-3 ulib apps wired into the embedded-app
> registry + the Apps menu. **Reversi/Othello** (`user/reversi.c`): full 8×8 disc-flip logic in all 8
> directions, pass-when-no-move handling, majority-wins endgame, and a positional-greedy AI (seeks corners,
> avoids the trap squares beside them); arrows move a cursor and legal moves are shown as `*`. **Lights Out**
> (`user/lights.c`): the 5×5 toggle puzzle, board generated by random presses from all-off so it is always
> solvable, with a move counter. Both verified in-guest (Reversi: placed a disc → flips + AI reply confirmed;
> Lights Out: board + toggle). All 29 `make check` suites still green.

> **(M706) NES emulator — a whole console, via vendored libxnes + a thin platform shim.** Following the
> DOOM/Quake recipe (vendor a portable core + a small OS shim + a freely-licensed game on the FAT disk),
> `user/nes/` now hosts **libxnes** (a pure-C99 NES core — CPU/PPU/APU/DMA/controller/cartridge + mapper
> bank-switchers, **MIT**) behind `nes_osdev.c`: it reads `GAME.NES` off the disk, opens a 256×240 window,
> and loops `drain raw scancodes → joypad 1 / step one frame / blit` (`xnes_get_pixel` is already
> `0x00RRGGBB` → straight into `sys_gfx_blit`; timing via `sys_uptime_ms`/`sys_sleep`; audio is a no-op sink
> for now, sound to follow like DOOM did). The shipped game is **Nova the Squirrel** (GPLv3 homebrew by
> NovaSquirrel, mapper 1/MMC1, CHR-RAM). Booting it took two fixes to libxnes itself, isolated by
> host-testing the core against the ROM (0 → thousands of rendered pixels, then visually confirmed as the
> title screen): **(1)** `xnes_mapper1_init` never set MMC1's power-on PRG mode, so both PRG slots mapped
> bank 0 and the reset vector at `$FFFC` read the wrong bank (blank screen) — now it powers up PRG-mode 3
> (last 16K bank fixed at `$C000`), the de-facto hardware default the reset-bit path already applied;
> **(2)** the NES-2.0 header branch lacked the CHR-RAM fallback the iNES-1.0 branch had, so a 0-CHR-ROM cart
> got a zero-length CHR buffer — now an 8 KB writable CHR window. A third, OS-side subtlety: the NES samples
> controller *state* once per frame, so a synthetic/quick key tap (QEMU's ~100 ms `sendkey` can collapse into
> a single emulated frame) would net to "released" and miss an edge-triggered "Press Start" — the shim now
> latches a same-frame tap as pressed for one frame while genuine holds pass through. Verified headlessly
> (`osdrive`): title → Main Menu (Start) → Level-Select world map with the character moving. All 29
> `make check` suites still green. The MMC1 + CHR-RAM fixes light up a large slice of the real NES library
> (Zelda, Metroid, Mega Man 2, …), so more `.nes` games are now nearly free to add.

> **(M687-M700) Spec-compliance probing arc — ~14 wrong-answer fixes the crash-fuzzers can't see.** After
> the async/Promise/fetch arc, semantic probing (build the engine `-DJS_HOSTTEST`, diff edge cases against
> spec) kept paying out — each a value real code depends on, none a crash: **Promise.any** + **thenable
> assimilation** (`Promise.resolve`/`await` a `{then}` object now runs its `.then`) (M687); **JSON.stringify**
> `toJSON()` hook + the ±Infinity sentinel → `null` (M691); **Array** `lastIndexOf(x, fromIndex)` honored its
> fromIndex (was ignored) + `[].reduce(fn)` with no init throws (M692); **ToNumber** `0b`/`0o` string prefixes
> (joined the existing `0x`) (M693); **String** `startsWith`/`endsWith` honor their position arg + `repeat(-1)`
> throws (M694); **`new Date(string)` + `Date.parse`** (ISO date-string parsing, M695); **regex replace
> `$<name>`** named-group substitution (M696). Then the OBJECT MODEL: **`B.prototype = Object.create(A.prototype)`**
> classic inheritance (reassigning `.prototype` was silently dropped, so new instances + `instanceof` lost the
> chain) (M698); **`obj.constructor`** + **`fn`/`Class.name`** (`new X().constructor.name`) (M699);
> **`new.target`** (abstract-class guard / factory / arrow-inherits) (M700). Golden-locked in
> `tests/js/suite-promise.js` (the fresh-arena overflow suite — `suite.js` is ~350KB from its 40MB cap).
> Recurring lesson: FUZZING finds crashes, PROBING finds wrong answers — pair them. Two consecutive zero-yield
> batches (control-flow, collections) confirmed those areas comprehensive. Remaining JS gaps are
> architectural/large: **BigInt** (arbitrary precision), and `extends` a native built-in like `Array`/`Map`
> (the copy-based class model can't make an instance a native-typed object — `extends Error` works because
> Error instances are plain).

> **(M679-M682) async / Promise runtime — the other big JS frontier, on a SYNCHRONOUS-resolution model.**
> The OS has no event loop and its I/O is blocking, so Promises settle eagerly: an executor runs immediately,
> resolve/reject settle on the spot, and `.then`/`await` read an ALREADY-settled state — covering the common
> synchronous-fetch patterns (`.then` chains, `await fetch()`, `Promise.all` over settled work) without a
> microtask queue. **M679** Promise core: new obj kind `V_PROMISE` (state+value in the promise's own `vals[]`
> so the shared `obj` struct grows by ZERO bytes — a +1 `val` field there overflowed the 40MB arena);
> resolve/reject capture their promise as a `V_BOUND` bound-arg (natives get no self pointer), so a stored
> resolver works; `then`/`catch`/`finally` with rejection propagation + returned-promise flattening;
> `Promise.resolve`/`reject`/`all`/`race`/`allSettled`; `instanceof Promise`. **M680** `async function` +
> `await` (gated by `g_in_async` so they stay ordinary identifiers elsewhere; `await` is a unary prefix, and an
> awaited rejection re-throws so `try/catch` works). **M681** async arrows, **M682** async methods (a shared
> marker disambiguates `async m(){}` from a member named `async`). All gated → non-async code byte-identical.
> Tests live in a SEPARATE golden run (`tests/js/suite-promise.js`, fresh arena) because the monolithic
> `suite.js` runs one 40MB arena to completion with only ~350KB headroom — appending there OOMs an unrelated
> case mid-run. Out of scope (inherent to the model): true concurrency / a never-resolved promise stays
> pending; real microtask ordering. **M684-M685** then made it usable for real network code: a `fetch(url)`
> global returning `Promise<Response>` (`{status, ok, text(), json()}`), capability-injected like storage/DOM
> (`js_set_fetch`), with the browser wiring `http_get`/`tls_get` (HTTP + HTTPS) via `browser_fetch` (its own
> scratch, never `b->raw`; parses the status line + strips headers). An HTTP error status resolves with
> `ok=false`; only a network failure rejects. Page scripts run on the main thread AFTER the worker fetched the
> page, so a JS fetch is a sequential blocking get (UI freezes for its duration — the sync-model cost). The JS
> half is host-tested via a mock backing; the real-network in-guest path mirrors the proven `worker_fetch` /
> `net_demo` code (not automatable — external reachability is non-deterministic). **M702-M703** extended it to
> **POST**: `fetch(url, {method, body, headers})` reads the options, and new `http_post`/`tls_post` send the
> body (Content-Type from headers, default text/plain) over HTTP and HTTPS — the TLS GET path stays
> byte-identical (a new POST branch shares the established session; no crypto change), so HTTPS GET can't
> regress. fetch is now a usable HTTP client (GET+POST, Response.text/json, async/await, Promise.all).

> **(M671-M674) Shell matcher host-testing + the last big JS gap (generators).** **M671-M673** extracted the
> shell's two pure matchers — the `grep` regex (`^ $ . * [..] \`) and the filename glob (`*`/`?`) — into
> `user/shgrep.h` and host-tested them (a 28th suite: regression + 500k fuzz, ASan/UBSan), bringing them into
> the codebase's extract-for-testability pattern (cssprop/url/htmlentity/…). **M674** then implemented JS
> **generators** (`function*` / `yield` / `yield*`), which had been a parse-abort that killed the entire page
> script. EAGER model: calling a generator runs its body to completion, collects every `yield` into an array,
> and returns that array (iterable via `for-of`, `[...]`, `.map`). Covers the common finite-generator uses; an
> infinite generator errors cleanly (arena-OOM, no hang) and manual `.next()` / the yield-expression value
> aren't modelled (eager has no resume point). All changes are GATED — `g_in_gen` keeps `yield` an ordinary
> identifier outside a generator body, `is_gen` gates the eval — so non-generator code is byte-identical
> (jstest golden unchanged, jssrcfuzz clean). **M676** then added generator METHODS (`class C { *g(){} }`,
> object `{ *g(){} }`, and computed `*[Symbol.iterator](){…}`) plus taught the iterator-protocol consumers
> (spread / for-of / Array.from) to accept a generator's array result — so `*[Symbol.iterator]()` custom
> iterables drive `[...obj]` / `for (x of obj)`. Generators are now comprehensive; only manual `.next()` /
> two-way `yield` values are out of scope (inherent to the eager, resume-point-free model).

> **(M660) The probing generalized past JS: the browser's URL resolver didn't canonicalize `./`/`../`.**
> `resolve_img_url` concatenated a relative `<img src>`/`<a href>` onto the base directory without RFC 3986
> remove_dot_segments, so `../up.png` on `http://a.com/dir/sub/p.html` became `…/dir/sub/../up.png` (a strict
> server/CDN 404s on a literal `../`). Added an in-place `norm_path` (collapse `/./`, pop a segment per `/../`
> clamped at root, leave `?query`/`#frag` verbatim) applied in all three branches (absolute / protocol-relative
> / relative). Unit-tested standalone over 18 cases; urltest's 400k fuzz stays clean (norm_path only shrinks
> within the bounded buffer); regression cases added. (Probing the colour parser, by contrast, found the muted
> named palette — red=CC0000 etc. — is a deliberate, test-locked design choice, not a bug: left as-is.)
> **A composable shell text-tool suite (M649-M650, M664-M665, M668-M669):** `grep` regex (`^ $ . * [..] \`),
> `sort -n`/`-r`/`-u` (numeric / reverse / unique), `uniq -c` (run counts), and a new `seq` generator — all
> verified in-guest, and they compose: `seq 5 | sort -nr` -> 5,4,3,2,1. **M662:** `http_find_loc` now trims trailing OWS from the `Location:` value (RFC 7230) so a
> redirect URL has no stray spaces. **M663:** pngenc test now validates png_encode output is a *standard* PNG
> (magic + chunk CRCs + zlib IDAT, via Python stdlib), mirroring the deflate runner's system-gunzip check.

> **(M649-M650) Shell `grep` gained regex (was literal-substring only).** `grep '^foo'` used to search for the
> four characters "^foo"; `grep 'a.c'` wanted a literal dot. Added the compact Kernighan/Pike matcher: `^`/`$`
> anchors, `.` any-char, `*` zero-or-more, `[..]` classes (ranges, negation, leading-`]` literal), and `\`
> escapes, honouring the existing `-i` case fold. A pattern with no metacharacters still behaves exactly like
> the old substring search, so existing greps are unchanged. Verified two ways: a standalone host harness
> (~37 cases incl. ranges/negation/`[..]*`/escape/case-insensitive, ASan/UBSan clean) and in-guest
> (`ls > gf.txt` then `grep ^R` -> only R-prefixed names, `5$` -> only lines ending in 5, `HEL.O` via `.`,
> `RE*GEX` via `*`, `^[RT]` via class+anchor). The matcher is bounded by pattern length (matchstar iterates,
> never recurses on the text), so no deep recursion on long lines.

> **(M640-M646) JS-engine semantic completeness via host probing — fuzzing finds crashes, probing finds
> WRONG ANSWERS.** The image-decoder audit proved the parsers were memory-safe; this arc asked the other
> question: does the JS engine compute the RIGHT result? Building it `-DJS_HOSTTEST` and diffing ~90 edge cases
> against integer-JS semantics found one real bug, then a whole cluster in the regex engine (whose regexfuzz
> only catches crashes, not wrong matches) — all fixed + locked in the golden suite. **M640** bitwise
> `& | ^ << >>` ran in raw int64 while `>>>` (M269) already used JS 32-bit semantics — now all coerce operands
> to int32 + mask the shift count (the canonical int32 "hello" hash 99162322 now computes; arithmetic stays
> exact int64). Then the regex engine: **M642** `\b`/`\B` word boundaries (were literal 'b'); **M643**
> `\1`..`\9` backreferences (were literal digits — now real, incl. backtracking `/(\w+) \1/`); **M644** the `m`
> (multiline) + `s` (dotall) flags, fixing a genuine bug where `^`/`$` were ALWAYS multiline (`/^world$/`
> wrongly matched "hello\nworld"); **M645** `(?:)` non-capturing groups (were capturing, mis-numbering `$1`) +
> `\xHH` hex escapes; **M646** lookahead `(?=)`/`(?!)` (zero-width, via the VM's own recursion — the
> `/^(?=.*\d)\w+$/` password idiom now works). The engine went from "common features only" to a genuinely
> useful subset: classes, quantifiers, greedy/lazy, alternation, named + non-capturing groups, backrefs, all
> anchors + word boundaries, both flags, and lookahead. Safe throughout (only suite.js uses regex/bitwise, all
> small/int32-range; shell grep has its own matcher); all 27 suites green incl. the in-guest browser.
> **Probing kept paying out (M652-M654, beyond regex):** **M652** the `new Date(y,m,d,…)` constructor stored
> its args raw while getTime/getDay used the exact civil calendar — so getDate/getMonth disagreed and the
> common `new Date(y,m+1,0)` "last day of month" idiom returned 0; now it normalises via a civil round-trip
> (month overflow carries the year, day 0 = last of prev month, 2100 correctly not a leap year). **M653**
> function declarations weren't hoisted, so a call textually before `function f(){…}` threw "undefined
> variable"; the block/program executor now defines them in a pre-pass (forward refs + that pattern work; var
> hoisting/expressions unchanged). **M654** class private fields `#x` were a no-op AND a privacy leak (`#x=5`
> wrote the public `x`, `this.#x` read undefined) — the lexer now keeps a leading `#` as part of the
> identifier, so declaration + access agree on "#x" and it stays distinct from public `x`. (Generators
> `function*` were unsupported at this point — later added in M674, eager model.) **Still more (M656-M658):** **M656**
> built-in Error subtypes (TypeError/RangeError/SyntaxError) are now `instanceof Error` (their ctors'
> parent_class links to Error), so the `catch(e){ if (e instanceof Error) … }` guard works; **M657**
> `JSON.stringify` honors a *function* replacer (was array-allowlist-only — `(k,v)=>…` now transforms/drops
> each node at every depth); **M658** `ToNumber(string)` skips leading whitespace, parses a `0x` hex prefix,
> and honors a leading `+`/`-` (`Number("  7  ")` was 0, `Number("0x10")` was 0), while keeping the engine's
> lenient trailing-char behavior + string `+` concat. Each fix is golden-locked — ~13 real JS/regex
> correctness bugs from one probing arc, none of which the crash-fuzzers could see.

> **(M634-M640) Test-hardening + a JS correctness fix: lock in this session's fixes, fuzz the untrusted-input
> parser paths the existing fuzzers couldn't reach, and fix a bitwise-operator divergence.** The data-loss bug class (below) was fixed + verified in-guest but had no
> host regression coverage, and an audit of the image decoders found them robust *by reading* yet under-fuzzed
> where it matters. **M634** adds host regressions (fs_test, which `#include`s fat32.c) for the two FS bugs:
> `rm <non-empty-dir>` is refused with NO cluster leak (df-checked — full reclaim only after a proper delete),
> and a name that 8.3-truncates ("dl.html"→DL.HTM, "verylongname.txt"→VERYLONG.TXT) reads back by its long
> name. **M635-M637** fix a shared gap in the image fuzzer: it prepends only a format magic + ≤91 random
> bytes, so the decoders' deepest untrusted-input code never ran (it needs a structurally valid header first).
> Now a real header is built and the *compressed payload* fuzzed: **M635** GIF LZW (variable-width codes, the
> dictionary, the KwKwK self-reference, the sub-block chain), **M636** JPEG Huffman→dequant→IDCT→YCbCr (a real
> embedded 8×8 baseline JPEG, its entropy scan mutated), **M637** PNG recon_filters (all 5 filter types) +
> expand_px (per colour type), via a STORED-deflate IDAT so inflate is a memcpy and the decode always reaches
> recon+expand on bytes we control. Two techniques made these TRUE tests, each verified by planting an OOB and
> watching the suite go red then restoring it: (1) buffers sized to the EXACT image — a shared 4MB scratch
> masks a write past the per-pixel cap, while tight malloc'd buffers put an ASan redzone right at the boundary;
> (2) small image dims so the output bound is hit constantly. The decoders all proved clean (the audit was
> right — they bound every dimension, table index, and output write); the lasting value is regression coverage
> for the exact paths a remote image/page can drive. **M639** extends this to the *animated*-GIF compositor
> (`gif_decode_anim` — multi-frame, the GCE, disposal restore-to-background, the per-frame snapshot; web-
> reachable yet previously undeclared in the harness): a known 3-frame decode + 120k mutated-frame iters into
> exact-size buffers (verified genuine — weakening the per-frame `il+iw>W` bound aborts in `gif_decode_anim`).
> **M640** is a real correctness fix the engine-probing turned up: the JS engine's `& | ^ << >>` operated in
> the raw int64 number while `>>>` (M269) used JS 32-bit semantics, so `1<<31` (gave 2147483648, not
> -2147483648), `1<<32` (no shift-count mask), and `0xFFFFFFFF|0` (gave 4294967295, not -1) diverged from
> every real JS engine and broke the idioms browser scripts depend on (`x|0` int-coercion, `(r<<16)|(g<<8)|b`
> packing, 32-bit string hashes). All bitwise ops now coerce operands to int32 + mask the shift count to & 31
> (arithmetic stays exact int64, like JS doubles); the canonical int32 hash of "hello" (99162322) now computes.
> Safe — the only bitwise in the tree is suite.js with int32-range values (golden byte-identical), no app uses
> JS bitwise. All 27 suites green (incl. the in-guest browser that runs the engine).

> **(M603-M630) A systematic data-loss/correctness bug class — found + fixed across the whole file surface
> via in-guest verification.** Verifying the FAT32 write path (a cp/edit/save round-trip) led into the
> shell's file commands and exposed a pervasive pattern: **fixed-size buffers silently truncating anything
> larger.** Consequences ranged from **silent data loss** — cp/mv truncated copies to 4KB (and mv then
> deleted the source); the editor (ESC always saves) rewrote any file >8KB as just its first 8KB; `>>`
> append rewrote the target as its first 8KB — to **wrong-but-authoritative results** — sha256/sha512/crc32
> hashed only the first 16KB; `cmp` reported "files are identical" for files that differed past 2KB;
> wc/grep/tail/sort/nl/uniq/hexdump processed only a 0.5–8KB prefix; gzip/gunzip/crypt capped at 16–256KB.
> Each now handles the whole file (a kernel `read_whole_file()` + a shell `slurp()`: a heap buffer that
> doubles until the read no longer fills it, capped at 32MB; sort/nl/uniq/tac also lost their fixed line caps),
> every fix verified in-guest against host tools / file sizes (e.g. sha256 of WALL.PNG matches `sha256sum`;
> `cmp` of two 14KB copies differing only at the end now reports the diff; `cat *.htm > f` captures all
> 71KB; gzip→gunzip of a 430KB WAV round-trips byte-identical). A **separate parsing bug** surfaced
> alongside: redirect parsing left the space before `>` in the command, so `cmd arg > file` passed `arg `
> (trailing space) and broke FAT32 lookups / streq commands — redirection only worked written tight
> (`cmd>file`). The pipe/redirect capture buffer also grew from a fixed 8KB to a growable heap buffer.
> Also **M603**: ~38 more CSS named colours (real pages rendered tomato/dodgerblue/limegreen as no colour).
> By M627 EVERY file-touching shell command handles whole files (M617-M619 tac/strings/tr/cut/fold, fold
> printing each wrapped line as it goes; M621 comm/paste; M622 diff's input; M625 base64 encode/decode;
> M626 the `js` command's source file + 1MB output; M627 todo's read-modify-write list). The one remaining
> cap — diff's 128-line LCS table — diff REPORTS it ("(diff truncated at 128 lines/file)"), so it's honest,
> not silent: **no silent truncation remains.** Verifying subdirectories then surfaced an unrelated FS bug:
> **M624** `rm <non-empty-dir>` deleted the directory but freed only ITS clusters, orphaning every child
> file's clusters (silent FAT corruption / space leak) — now it refuses a non-empty directory (df confirms
> clusters are freed exactly on delete). **M629** then bumped wget's fixed 16KB download buffer (it saved
> files truncated) to 1MB; downloading a 20KB page surfaced yet another: **M630** the read path (dir_find)
> didn't 8.3-normalize the lookup name the way write/delete do, so a file written as e.g. "dl.html" (stored
> DL.HTM) couldn't be read back by that name — now it matches the truncated form too. A clean cascade: one
> round-trip test surfaced a whole bug class plus several adjacent FS/parsing bugs. All 27 suites green
> throughout.

> **(M599) Real bug found by in-guest verification: Quake didn't launch in the default RAM config.** While
> screenshot-verifying the marquee features, DOOM rendered but **Quake produced no window and no crash** —
> its init silently failed. Root cause: Quake needs its 18 MB PAK + a multi-MB hunk on top of the kernel
> (whose BSS now includes the 40 MB JS arena), which QEMU's **~128 MB default can't satisfy** — the alloc
> fails and Quake exits cleanly (window reaped, no fault). DOOM (4 MB WAD) fit, so only Quake broke. Fix:
> `-m 256M` everywhere the OS launches (Makefile run/test, the three in-guest test scripts, osdrive.py).
> Confirmed in-guest: Quake now renders E1M1 + HUD, **and DOOM+Quake run concurrently** (the M519 feature,
> now actually exercised). All 27 suites green at 256M. **M601** then fixed the *silent*-ness that made
> this hard to find: `app_spawn_named` now logs load failures, and `app_sbrk` logs OOM — launching Quake
> at 128M now prints `[app] 'Quake' out of memory: sbrk(33558528) failed …` (its ~32 MB hunk), instead of
> nothing. A clean example of verification surfacing a real, high-value regression host tests couldn't —
> plus a diagnostic so the next one isn't silent.

> **(M590-M592) Real-page rendering polish + DE touch, found by reviewing/rendering real sites.** Continuing
> the content-parser review: **M590** the browser now honors the `font:` shorthand (`style="font: bold
> 14px Arial"` previously set neither weight nor size); **M591** `uni_to_ascii` folds common math/arrow
> symbols (≤ ≥ ≈ → ← ↑ ↓ −) to nearest-ASCII instead of dropping them to a space (they appear on technical
> pages) + the matching `&le;`/`&ge;`/`&asymp;` entities. **M592** (DE) clicking the taskbar clock opens
> the Calendar. Verified in-guest over real TLS: **example.com** and **danluu.com** (a content-rich blog,
> 21 KB) render correctly; an **unreachable** host (gnu.org, blocked from this sandbox) fails *gracefully*
> ("failed" status, no hang/crash) — confirming robust network handling, not a bug. `make check` stays
> **27 suites**, all green.
>
> **(M594-M595) Tooling + shell completeness.** **M594** `tools/osdrive.py`'s `type` now sends uppercase
> + full punctuation (`:` `?` `(` …) via shift-combos, so it can type URLs/code, not just lowercase
> words. **M595** the shell gained `touch` (the one missing common file-op; creates a 0-byte file,
> never truncates an existing one). With osdrive able to type URLs, the browser's **address-bar
> navigation** path is now verified too (`e` → type `file:table.htm` → Enter renders it) — so every
> browser input path is confirmed in-guest: link-click, address-bar typing, page-script (`document.write`,
> ASCII table), and **interactive onclick→JS→DOM→re-render with persistent state** (Rock-Paper-Scissors:
> click → CPU `Math.random` move → score persists), alongside inline `data:` images and real-TLS sites.

> **(M566, M580-M588) Browser untrusted-parser fuzzing — every scanner extracted + fuzzed, 4 CSS fixes.**
> The browser parses a hostile server's bytes on the guard-page-less kernel stack; all the cleanly-
> separable scanners are now lifted into their own `.c` and host-fuzzed under ASan/UBSan (the M524/M546
> pattern), each a verified oracle (loosen a bound → ASan abort at the exact line): **M566** `htmlattr.c`
> (tag attribute scanners); **M580** `url.c` (`url_split`/`resolve_img_url` — address bar/`<a href>`/
> `<img src>`/redirects); **M581** `color.c` (`parse_color` — `#hex`/`rgb()`/`hsl()`/named, clamped int
> math); **M583** `cssprop.c` (`style_prop` — the inline-style declaration scanner all per-property style
> helpers build on). Each extraction is verbatim (diffed byte-identical) and verified behavior-preserving
> in-guest. The review/fuzz then surfaced + fixed **four** real CSS-rendering gaps (all in the shared
> path, so they fix both inline `style=""` and `<style>` rules): **M584** `style_prop` accepts whitespace
> before the `:` (valid CSS `prop : value`); **M585** it honors the cascade (a later duplicate wins,
> `color:red;color:blue` → blue); **M587** it strips the `!important` priority marker (`color:red
> !important` was silently dropped — the value failed every parser); **M588** added `white` (!) + ~19
> common named colours (`white`/`aqua`/`lightgray`/… were missing, so `color:white` parsed to nothing).
> All purely additive — the home page renders byte-identically; each locked by a regression case + the
> fuzzers staying ASan/UBSan-clean. The main `parse_html` tokenizer stays too coupled to `browser_t` to fuzz in
> isolation, so it's guarded end-to-end by `browsertest`. `make check` is now **27 suites** (24 host + 3
> in-guest). Marquee features re-confirmed in-guest by framebuffer screenshot this session: the desktop,
> the browser (home + real HTTPS), Mandelbrot, the System Monitor, every window gesture — and **DOOM**
> (E1M1 + HUD).

> **(M574-M578) JS engine — closed the documented gaps; coercion + regex + JSON now spec-complete.**
> With QEMU back (so each change is verifiable in-guest), a focused pass finished the JS engine's long-tail:
> **M574** ToPrimitive `toString` (string coercion of objects/classes: `""+o`, `String(o)`, template
> literals, `Array.join`); **M576** ToPrimitive `valueOf` (numeric coercion: `obj-1`, `Number(obj)`, `+obj`,
> comparisons; valueOf-first, toString-parse fallback) — together **completing ToPrimitive**, the one gap
> prior sessions deferred *specifically* for lack of in-guest verification; **M577** `match.groups`
> (named captures `(?<n>…)` were added in M543 but the names discarded) **and** `match.index` — via a new
> `match_props` side-object on the result array, so positional `m[1]` and ordinary arrays are unaffected;
> **M578** `JSON.parse` reviver (the symmetric counterpart to the stringify replacer) + an arena bump
> (32→40 MB, the parser AST + the growing test suite). Each landed with the same rigor: host cases incl.
> edges, the jstest golden UNCHANGED for every prior line, the relevant fuzzer (regex/json/source) clean,
> and **verified in-guest** (the shell `js` demo now shows toString/valueOf/regex-groups/reviver from the
> in-kernel engine). A broad re-probe (~30 modern features) now finds only architectural ceilings left —
> generators/async/Promise (need coroutines + an event loop) and real BigInt (arbitrary precision); the
> integer engine and the synchronous standard library are otherwise complete.

> **(M562-M570) Tooling + window gestures + browser-parser fuzz + a gated browser test — the in-guest arc.**
> **M563** promoted the throwaway driving scripts into a committed tool, `tools/osdrive.py`
> (boot headless, inject keys + absolute-mouse clicks/drags, screenshot) — the durable enabler for all of
> the below. **M564** aero-snap: drag a window to the top edge to maximize or a side edge to tile that
> half (completing F4/F5/F6/double-click). **M565** made the F1 help overlay modal to the mouse. **M566**
> (security, not DE) extracted the browser's HTML attribute scanners — `find_attr`/`has_attr`/`attr_int`/
> `find_href`, which walk a hostile server's tag bytes — into `kernel/htmlattr.c` and host-fuzzed them
> (400k random + truncations/corruptions, ASan/UBSan, verified oracle), the M524/M546 pattern. **M568**
> added a 3rd in-guest gate, `browsertest`: it launches the Browser from the Apps menu and asserts its
> network-free home page rendered (≥50k white pixels) — the end-to-end guard for `parse_html`, the HTML
> tokenizer that's too coupled to `browser_t` to fuzz in isolation. **M569** the System Monitor now shows
> FAT32 disk usage (cached, since `vfs_df` scans the FAT). **M570** type-to-jump in the 34-entry Apps menu.
> `make check` is now **24 suites** (21 host + 3 in-guest). Verified in-guest with osdrive this arc: the
> browser renders its home page AND navigates to a real **https://example.com** over the from-scratch TLS
> stack, the System Monitor (with disk), and every window gesture — all by framebuffer screenshot, no display.

> **(M556-M561) Desktop-environment polish — driven + verified in-guest.** With QEMU back (above), used
> the new full headless control — keyboard via HMP `sendkey` AND mouse via QMP `input-send-event`
> (absolute tablet, 0..32767), plus `screendump` — to improve the DE the user actually asked for ("idc
> about music i want a good DE") and *visually verify each change*. **M558** F1 keyboard-shortcut help
> overlay (the WM had grown F2/F3/F4/F5/F6/F8/F9/F12 but they weren't all surfaced anywhere). **M559**
> taskbar clock now shows date + time (`YYYY-MM-DD HH:MM:SS`), not just time. **M560** fixed the chip
> hit-test that M559's wider clock pill broke (extracted one `clk_pill_w()` so render + hit-test can't
> drift). **M561** double-click the title bar to maximize/restore — which exposed and fixed a pre-existing
> drag bug (a bare title-bar click cleared the `maximized` flag without restoring geometry; now the move
> handler acts only past a 3px threshold, and dragging a maximized window restores its size and follows
> the cursor). **M556** also fixed a stale `tls.h` comment + **M557** a host-build break (the `nettest`
> harness needed TLS stubs after M555 added a tls_get call to net.c). Confirmed in-guest this arc: the
> desktop, browser start page, Mandelbrot, the help overlay, snap-tiling, maximize/restore, and window
> dragging all render/behave correctly. **The screenshot+input workflow is the durable win** — any
> graphical regression is now reproducible and reviewable headlessly.

> **(M549-M555) QEMU is UNBLOCKED — in-guest verification restored + gated.** The single biggest
> change this session wasn't a feature, it was discovering that **QEMU runs again** (v10.2.2). The
> previous ~30 milestones (M520-M548) all landed "host-verifiable only" because earlier sessions hit
> a QEMU launch failure (SIGSTKFLT) that made in-guest testing impossible. That premise is now stale.
> The OS boots end-to-end (preemption, isolation, PCI, e1000 networking incl. a real HTTP GET to
> example.com, FAT32, USB tablet), the **desktop paints** (verified by framebuffer screenshot), and the
> **browser renders its CSS-styled start page** — and the **shell window confirms ring-3 userspace
> actually executes in-guest**, all captured headlessly. Turned this back into a *gate*, not a one-off:
> **M549** the `make test` smoke boot now exercises the AC'97 audio bring-up (`-audiodev none`, no host
> sound server); **M550** added `boottest` — boots the real `kernel32.elf` headless and asserts all 9
> bring-up markers print with no crash (the whole driver stack, vs the host suites' one-`.c`-in-isolation
> model), verified as a real oracle (`QEMU=true` → every marker MISSING → FAIL); **M552** added `gfxtest`
> — captures the VGA framebuffer via the QEMU monitor's `screendump` and asserts the desktop actually
> painted (1024×768, ≥40 colors, no all-black hang), covering desktop.c/fb.c/fbcon.c/font.c/vga.c which
> had *zero* in-guest coverage; **M553** made `boottest`'s real-internet GET non-fatal so the gate stays
> green offline. **M551** cleaned up a GCC `malloc`-size false-positive in the M548 fuzzer. Then **M555** added a
> boot-time **TLS 1.3 HTTPS self-test**: the kernel now does a real HTTPS GET to example.com alongside
> the existing HTTP one, and in-guest it completes the *whole* from-scratch handshake — parses the 4-cert
> chain (example.com → Cloudflare ECC CA → SSL.com Transit → SSL.com ECC Root), matches the hostname,
> verifies 3/3 issuer signatures, anchors to a trusted root, passes `CertificateVerify`, and fetches the
> page (`200 OK`). The crown-jewel TLS stack (X25519, AES-GCM/ChaCha20-Poly1305, X.509 path validation,
> ECDSA/RSA over our own bignum) is now verified end-to-end at every boot (a soft `boottest` marker),
> and `boottest` polls COM1 to exit ~when the desktop launches (~5s) with a 25s safety net. `make check`
> is now **22 suites** (20 host + 2 in-guest); both new in-guest tests SKIP cleanly where QEMU/socat/
> python3 are absent. The framebuffer-screenshot + HMP-input-injection technique (drive the Apps menu via
> `sendkey`, then `screendump`) is now a general way to verify *any* graphical feature headlessly — used
> this session to confirm the desktop, the CSS-styled browser start page, and the Mandelbrot app all
> render correctly in-guest.

> **Status (546 milestones) — DOOM *and* QUAKE RUN, with sound.** OS-DEV now runs two real id Software
> games as windowed ring-3 apps: **DOOM** (graphics, keyboard, mouselook, sound effects, and music) and
> **Quake** — the true-3D successor, software-rendered (actual Half-Life is a closed Win32/GoldSrc title
> needing a GPU and proprietary assets, so Quake is the realistic "modern game"). Both load their shareware
> data from the OS's own 64 MiB FAT32 disk, and they run **concurrently** (M519 added FXSAVE/FXRSTOR FP
> state save so two floating-point apps don't corrupt each other). Getting here (M495-M519, ~25 milestones)
> built whole new subsystems: a **userspace heap** (malloc/sbrk, fuzz-tested), a **graphics window API**
> (real per-window pixel canvases — the end of text-grid-only apps), **raw make/break keyboard** + **mouse**
> (absolute + relative/mouselook), **FPU/SSE**, an **AC'97 audio stack** (PCM, a WAV parser [fuzz-tested],
> streaming, **background music**, a real WAV **jukebox**, DOOM SFX + a MUS music synth), and a **ms clock**.
> Plus polish: a mouse paint app, a mouse-zoom Mandelbrot, and a disk-loaded desktop wallpaper. The two
> game ports reused the proven approach: vendor (doomgeneric / quakegeneric) + a platform layer over the
> syscalls + a from-scratch libc shim, with subagents doing the bulk under review. **The "best little
> from-scratch OS" now runs DOOM and Quake — alongside its own browser, TLS stack, and JS engine.**
> **(M541-M546) More JS compat + untrusted-parser fuzzing.** Continuing the host-verifiable arc:
> **M541** `new Date(ms)` / `new Date(y,mo,d,…)` honor their argument (were snapshotting "now") +
> real `toISOString`; **M542** array `indexOf`/`includes`/`lastIndexOf` match objects by identity
> (`===`, also null/undefined) + Date `getUTC*`/`toLocale*`; **M543** regex named capture groups
> `(?<name>…)`; **M544** `class X extends Error` (custom error classes — super(msg) sets message,
> instanceof both ways) + `Error.toString`; **M545** `JSON.stringify` array replacer (key allowlist).
> Then **M546** extracted the HTML entity decoder (`decode_entity`, untrusted page bytes) into
> kernel/htmlentity.c and fuzzed it — the 4th untrusted-parser fuzz suite this arc after JSON.parse,
> the regex engine, and the full JS source pipeline (M537-M539). `make check` is now **20 suites**,
> each new fuzzer verified as a real oracle (removing a bound → ASan abort at the exact line). The JS
> engine probed clean across ~130 features/edges; the one common remaining gap, ToPrimitive calling a
> user object's valueOf/toString, was deferred as too risky to land without in-guest verification (it
> touches the coercion core). **[Update: M574 landed the toString half](#)** — string coercion (`""+o`,
> `String(o)`, template literals, `Array.join`, `print`) now calls a user object's own/inherited
> `toString` (verified in-guest once QEMU was back; golden unchanged + fuzz-clean + the depth-guarded
> getter call path), and **M576 then landed the `valueOf` half** (numeric coercion in `to_num`:
> `obj-1`/`Number(obj)`/`+obj`/comparisons; valueOf wins, toString-parse fallback) — so **ToPrimitive is
> now complete**. Generators were then the sole documented JS ceiling (added M674, eager model). All four-way
> verified (host + golden + fuzz + in-guest).
> **(M529-M539) JS engine real-site compatibility + untrusted-input fuzzing.** Continuing the
> QEMU-blocked session in host-verifiable territory, a deep pass on the from-scratch JS engine (which
> powers the browser) — probed against ~110 modern features/edges, found + fixed 8 real gaps, each
> locked into the `jstest` golden: **M529** `Infinity` was an undefined-variable THROW (now an INT64_MAX
> sentinel — the engine is integer-only) + `toFixed` decimal padding + `flat(Infinity)`; **M530**
> `normalize`/`concat`/`toLocale*` + `replaceAll(regex)` (+ arena 20→26 MB as the suite grew); **M531**
> `JSON.stringify` omits undefined/function object props (was emitting null — could corrupt server
> payloads); **M532** regex **lazy quantifiers** (`/<.+?>/`), `split()` capture-group splicing, and
> `Array.join` rendering undefined/null as `""`; **M533** `Math.max()/min()` identity elements + global
> `isFinite`; **M534/M535** **`let`/`const` per-iteration binding** in `for`/`for-of`/`for-in` (the
> defining reason `let` exists — closures in loops now capture each iteration; with a closure-detection
> optimization so big loops stay allocation-free on the GC-less arena); **M536** array elision `[1,,3]`
> (was a parse-abort). Generators (`function*`) were then the one unsupported feature (the concern noted here —
> "an eager hack would hang on an infinite generator" — was exactly what M674 resolved: the eager model errors
> via arena-OOM, never hangs).
> Then **M537-M539** fuzzed every untrusted-input path in js.c under ASan/UBSan — `JSON.parse`, the
> **regex** engine (ReDoS shapes + malformed patterns; this engine's history records 2 critical matcher
> stack-overflows), and the **full source parse+run pipeline** — each #including js.c via a new
> `JS_NO_MAIN` guard, each verified as a real oracle (removing a bound → ASan abort at the exact line).
> `make check` is now **19 suites**. All host-verified; QEMU stayed environment-blocked all session.
> **(M522-M527) Robustness + trust-boundary hardening.** A focused session on
> the parts where a bug is silent kernel corruption. **M522:** the Apps menu had
> grown to 34 entries and overflowed the top of the screen (clipping Browser/
> Shell/Clock off-screen, unreachable) — reworked into a 2-column menu (render +
> mouse hit-testing + keyboard left/right column nav), headroom now ~60 entries.
> **M523:** the ELF64 loader (the ring-3 trust boundary) had no host test and
> interleaved untrusted-header validation with the page-mapping writes — split
> out pure `elf_check_header`/`elf_check_phdr` validators (added an OOM guard so
> a failed frame alloc never maps physical 0) and added an ASan/UBSan fuzz suite
> (truncations + corruptions + 200k random buffers + a full load round-trip via
> an mmap-backed guest-memory stub). **M525:** that suite now also loads every
> shipped app binary (all 29) through `elf_load` as a linker/toolchain
> regression guard. **M524:** extracted the HTTP/1.x response parsers (chunked
> decode — which memmoves with attacker-controlled hex sizes — + header scans)
> from browser.c into a testable `http.c` and fuzzed them (regression + 400k
> random). **M526:** the kernel heap (`kmalloc`/`kfree`, underlies every kernel
> allocation) was untestable on host (higher-half base) — parameterized the base
> (kernel-neutral) and added a 400k-op torture test with per-block pattern +
> free-list tiling invariants. **M527:** brought tests/README current (13->16
> suites; each new harness verified to fail when its guard is removed). All 16
> host suites pass. *In-guest QEMU verification was blocked this session — the
> environment terminates QEMU (SIGSTKFLT) on launch — so this session stayed in
> host-verifiable territory; the M522 menu fix is verified by inspection + build.*
> **(M495-M504) DOOM:** OS-DEV runs **id Software's DOOM** (the shareware
> doomgeneric port) as a windowed ring-3 app: it loads the IWAD from our own FAT32 disk, renders E1M1 in a
> crisp 640x400 window at ~70 fps, and is keyboard-playable through its menus into a live game. Getting
> there meant building six new OS capabilities first, each shipped + verified on its own (M496-M501):
> a **userspace heap** (SYS_sbrk + malloc/free/realloc, host-fuzzed under ASan), a **monotonic ms clock**,
> a **graphics window API** (real per-window pixel canvases the compositor blits — the end of
> text-grid-only userspace), **raw make/break keyboard events** for games, **FPU/SSE** enablement, and a
> **16 MiB disk** holding the ~4 MB WAD (a full multi-MB FAT32 read verified byte-exact by CRC-32). Then
> M502 vendored doomgeneric with a from-scratch **libc shim** + a **platform layer** over those syscalls
> (the one real bug: `_start` must force-align the stack or DOOM's first SSE `movaps` #GPs), and M503 added
> compositor integer-upscaling so the native 320x200 render shows at 2x. M495 (`file`, a magic-byte type
> identifier) opened the session. **All 12 host suites pass. The "best little from-scratch OS" now runs a
> real, beloved game — alongside its own browser, TLS stack, and JS engine.**
> M494: **`ls` shows the whole directory (was capped at 32)** — a real bug:
> SYS_list enumerated only 32 entries (vfs_dirent ents[32] on the stack), so with ~100 files now on the
> disk (~60 INDEX-linked .htm demos + baked images/archives + SHOT screenshots + extracted files) `ls`,
> GLOB expansion, and the browser file list all silently saw ~1/3. Fix: cap 32→256 using a STATIC array
> (256*68B too big for the 16KB kernel stack; concurrent-ls race is benign — bounded + NUL-terminated
> names) + a defensive j<63 name bound; shell ls buffer 1024→8192 + glob listing 2048→8192. Verified
> in-OS: `ls | wc -l` → 108 (was 32); `ls | grep TGZ` → finds TEST.TGZ (previously invisible). Files:
> kernel/syscall.c, user/shell.c. **A quality/bug-fix found while testing the archive work — the disk grew
> past the old cap. "Improve what needs it most." 17 milestones this session (M478-494).**
> M493: **`tar` — extract `.tar` + `.tar.gz`** — completes the archive suite
> (.gz/.zip/.tar.gz). kernel/tar.c: simple ustar 512-byte-header parsing (name@0, octal size@124, type@156),
> bounded (data span checked vs len, walk always advances), I wrote it directly (simple). SYS_untar(41):
> if the file starts with the gzip magic (1f 8b) → gz_inflate first → tar_extract — so .tar.gz/.tgz works
> in ONE step. REUSES gz_inflate + the unzip_emit 8.3-mangling callback (minimal new code). shell `tar <f>`.
> New tartest in `make check` (python-tarfile archive extracted exactly + truncation/corruption/garbage
> fuzz, ASan clean). Verified in-OS: `tar TEST.TGZ` → 3 files byte-exact (incl. an 800-byte deflate-in-tar
> entry, confirmed via disk). Files: kernel/tar.c, tar.h, syscall.c, shell.c, ulib.*, Makefile, tools/
> test.tgz, tests/tar/*. **11 host suites. ARCHIVE/COMPRESSION SUITE COMPLETE: gunzip/gzip/unzip/tar+PNG,
> all from-scratch on one inflate/deflate core. This session (M478-493, 16 milestones): image arc, markdown
> +GFM, CSV, Wordle, and the whole compression/archive subsystem — far past the prior 'saturation'.**)
> M492: **`unzip` — extract `.zip` archives** — completes the archive story
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
