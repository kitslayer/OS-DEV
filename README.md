# OS-DEV

A from-scratch x86_64 operating system, written in C + a little assembly,
booted via Multiboot under QEMU.

## Status

✅ **178 milestones complete — a fully keyboard-drivable graphical desktop OS with its own from-scratch JavaScript interpreter (with full class-based OOP — classes, `extends`, `super` — and modern ES6 syntax — spread/rest `...`, destructuring, arrow functions, template literals — atop a Math/JSON/String/Array standard library, runnable from the shell *and* inside web pages via `<script>`), whose web browser now browses the real HTTPS web (validated live on example.com, gnu.org, the NPR text news site, and danluu.com) — verifying the server's CertificateVerify signature, the cert chain's issuer links, and anchoring the chain to a baked-in trusted root CA (NPR → R13 → ISRG Root X1), all with from-scratch ECDSA/RSA/X.509 crypto — following links across pages (a from-scratch TLS 1.3 client, also wired to the shell's `get`/`wget`) and renders every common image format — PNG (incl. interlaced), animated GIF, and baseline-&-progressive JPEG — all inline and from scratch, plus colour text, a hierarchical filesystem, ten userspace apps (six games incl. Tetris/Breakout/Minesweeper, a text editor, …), a system monitor, a complete verified crypto + TLS toolkit (SHA-256/AES-GCM/ChaCha20-Poly1305/HKDF/X25519/RSA/ECDSA/X.509), and a real taskbar. It even loads and runs programs from its own disk.**
OS-DEV goes from power-on to a themed windowing **desktop environment** that
hosts **actual ring-3 programs as windows**: 64-bit long mode, interrupts,
physical/virtual memory + a heap, **preemptive** multitasking with **sleep/wake**
and **per-process isolation**, ring-3 userspace + syscalls, a **read-write
FAT32** filesystem on its own ATA driver, **PCI**, an **e1000 NIC**
(ping + **DNS resolve**), **framebuffer graphics**, mouse + **USB absolute
pointer**, **PC-speaker sound**, a **real-time clock**, and a **window manager**
(drag/resize/start-menu) hosting real userspace programs — a **shell**, a live
clock, and a **graphical web browser** that fetches and renders real web pages
over a **from-scratch TCP/HTTP stack** (chunked-encoding aware). The browser is
**fully keyboard-driven** — links (Tab/Enter), in-page find (`\`), bookmarks
(`a`), history/back, save, scroll — and renders headings, **lists, tables,
`<pre>`, `<img>` alt text, form fields, and HTML entities**. The desktop adds
keyboard window management (F2 focus, F4 maximize), an interactive **Files**
launcher, and **terminal scrollback**; and the OS can `run` an **ELF program
loaded from the FAT32 disk**, not just the kernel-embedded ones:

![OS-DEV's browser rendering info.cern.ch, the first website ever](docs/osdev-browser-cern.png)

Where to go next: **[WHATS-NEXT.md](WHATS-NEXT.md)**. The honest take on the
long-term goals (browser, music, Claude Code): **[GOALS.md](GOALS.md)**.

## Quick start

```sh
make          # build the kernel + userspace shell + FAT32 disk image
make run      # boot in a QEMU window (VGA output)
make test     # boot headless, capture serial output, exit after 5s
make clean
```

Drive the shell over the serial line (so it works headlessly):

```sh
(sleep 1; printf 'help\nls\ncat hello.txt\nexit\n') | \
  qemu-system-x86_64 -no-reboot -kernel build/kernel32.elf \
    -drive file=build/fat.img,format=raw,if=ide -display none -serial stdio
```

## How it boots

1. QEMU's built-in Multiboot loader (`-kernel`) finds the multiboot header in
   `boot/boot.asm`, loads the kernel at physical 1 MiB, and jumps to `_start`
   in **32-bit protected mode**.
2. `_start` (32-bit) verifies multiboot + long-mode support, builds page tables
   that identity-map the low 1 GiB, enables PAE + long mode + paging, loads a
   64-bit GDT, and far-jumps into 64-bit code.
3. The 64-bit entry sets up segments + stack and calls `kmain()` (C).

**The 32-bit-container trick:** QEMU's multiboot loader refuses an ELF64
("give a 32bit one"). So we link as a real **ELF64** (`build/kernel.elf`, keep
for `gdb`), then `objcopy` it into a **32-bit ELF container**
(`build/kernel32.elf`) the loader accepts — the 64-bit code inside is untouched.
No GRUB, no ISO, no `xorriso`.

## Layout

```
boot/boot.asm        multiboot header + 32->64-bit long-mode trampoline
kernel/              the kernel
  kmain.c            entry; wires every subsystem together
  console.c vga.c serial.c     text output + kprintf
  gdt.c idt.c interrupts.c pic.c   descriptor tables + interrupts
  timer.c keyboard.c           PIT + PS/2 (and the input queue)
  pmm.c vmm.c kheap.c          physical / virtual memory + heap
  task.c                       kernel threads + scheduler
  elf.c syscall.c              ELF loader + syscall dispatch
  ata.c fat32.c vfs.c          disk driver + filesystem + VFS
  asm/                         context switch, ISR stubs, usermode, user blob
  include/                     kernel headers
user/                ring-3 programs: ulib (libc) + shell
tools/mkfatfs.c      host-side FAT32 image builder
linker.ld            kernel ELF layout (loaded at 1 MiB)
Makefile             build / run / test
docs/                a learning write-up for every milestone
```

## Documentation

A standalone explainer for each milestone — read them in order to learn how the
whole thing works:

| # | Topic | Doc |
|---|-------|-----|
| 0 | Booting to 64-bit long mode        | [docs/00](docs/00-boot-and-long-mode.md) |
| 1 | Terminal driver + `kprintf`        | [docs/01](docs/01-terminal-and-kprintf.md) |
| 2 | Interrupts (GDT/TSS, IDT, PIC)     | [docs/02](docs/02-interrupts.md) |
| 3 | Timer + keyboard                   | [docs/03](docs/03-timer-and-keyboard.md) |
| 4 | Physical memory manager            | [docs/04](docs/04-physical-memory.md) |
| 5 | Virtual memory + higher-half       | [docs/05](docs/05-virtual-memory.md) |
| 6 | Kernel heap                        | [docs/06](docs/06-kernel-heap.md) |
| 7 | Multitasking + scheduler           | [docs/07](docs/07-multitasking.md) |
| 8 | Userspace, syscalls, ELF loader    | [docs/08](docs/08-userspace.md) |
| 9 | libc + shell                       | [docs/09](docs/09-libc-and-shell.md) |
| 10| VFS + FAT32 filesystem             | [docs/10](docs/10-vfs-and-fat32.md) |
| 11| Preemptive scheduling              | [docs/11](docs/11-preemptive-scheduling.md) |
| 12| PCI bus enumeration                | [docs/12](docs/12-pci.md) |
| 13| NIC driver + ARP + ping            | [docs/13](docs/13-networking.md) |
| 14| Framebuffer graphics + font        | [docs/14](docs/14-framebuffer.md) |
| 15| Graphical console + real font      | [docs/15](docs/15-graphical-console.md) |
| 16| PS/2 mouse + cursor                | [docs/16](docs/16-mouse.md) |
| 17| Window manager / compositor        | [docs/17](docs/17-window-manager.md) |
| 18| Desktop apps + widgets             | [docs/18](docs/18-desktop-apps.md) |
| 19| USB tablet (absolute pointer)      | [docs/19](docs/19-usb-tablet.md) |
| 20| Resizable windows + start menu     | [docs/20](docs/20-resize-and-start-menu.md) |
| 21| Per-process address spaces         | [docs/21](docs/21-process-isolation.md) |
| 22| Userspace apps as windows          | [docs/22](docs/22-userspace-apps.md) |
| 23| Sleep/wake (blocking)              | [docs/23](docs/23-sleep-wake.md) |
| 24| Visual polish (theming)            | [docs/24](docs/24-visual-polish.md) |
| 25| Real-time clock (RTC)              | [docs/25](docs/25-rtc.md) |
| 26| PC speaker sound                   | [docs/26](docs/26-pc-speaker.md) |
| 27| System commands (mem/clear/reboot) | [docs/27](docs/27-system-commands.md) |
| 28| FAT32 write (saving files)         | [docs/28](docs/28-fat32-write.md) |
| 29| Networking from the shell (ping/DNS) | [docs/29](docs/29-shell-networking.md) |
| 30| Userspace text editor              | [docs/30](docs/30-text-editor.md) |
| 31| File delete (`rm`)                 | [docs/31](docs/31-file-delete.md) |
| 32| Process spawning + multiple programs | [docs/32](docs/32-process-spawning.md) |
| 33| TCP + HTTP GET (fetch real web pages) | [docs/33](docs/33-tcp-http.md) |
| 34| **Graphical web browser** (HTML render) | [docs/34](docs/34-browser.md) |
| 35| Clickable links (follow + URL resolution) | [docs/35](docs/35-clickable-links.md) |
| 36| Non-blocking (async) page loads        | [docs/36](docs/36-async-browser.md) |
| 37| Richer HTML rendering (title/lists/hr) | [docs/37](docs/37-richer-rendering.md) |
| 38| Browser history / Back                 | [docs/38](docs/38-browser-history.md) |
| 39| `browse <url>` shell command           | [docs/39](docs/39-browse-command.md) |
| 40| Concurrency hardening (browser worker) | [docs/40](docs/40-concurrency-hardening.md) |
| 41| Browser start page + bookmarks         | [docs/41](docs/41-start-page.md) |
| 42| Save a web page to disk (`s` → PAGE.TXT) | [docs/42](docs/42-save-page.md) |
| 43| FAT32 subdirectories (mkdir/cd/pwd)    | [docs/43](docs/43-subdirectories.md) |
| 44| Calculator userspace app               | [docs/44](docs/44-calculator.md) |
| 45| Browser opens local files (`file:…`)   | [docs/45](docs/45-local-files.md) |
| 46| Filesystem write hardening (review fix) | [docs/46](docs/46-fs-write-hardening.md) |
| 47| `cp` / `mv` shell commands             | [docs/47](docs/47-cp-mv.md) |
| 48| `tree` recursive directory listing     | [docs/48](docs/48-tree.md) |
| 49| Taskbar window list (click to focus)   | [docs/49](docs/49-taskbar.md) |
| 50| `ps` process list                      | [docs/50](docs/50-ps.md) |
| 51| Browser bold & italic rendering        | [docs/51](docs/51-emphasis.md) |
| 52| Compositor scene-cache (optimization)  | [docs/52](docs/52-compositor-cache.md) |
| 53| Arrow-key support (browser scroll)     | [docs/53](docs/53-arrow-keys.md) |
| 54| Shell command history (↑/↓)            | [docs/54](docs/54-command-history.md) |
| 55| Snake game + non-blocking input        | [docs/55](docs/55-snake.md) |
| 56| `df` + input review fixes              | [docs/56](docs/56-df-and-fixes.md) |
| 57| Graphical text editor                  | [docs/57](docs/57-editor.md) |
| 58| `find` recursive search                | [docs/58](docs/58-find.md) |
| 59| Browser follows HTTP redirects         | [docs/59](docs/59-redirects.md) |
| 60| 2048 game                              | [docs/60](docs/60-2048.md) |
| 61| `hexdump` command                      | [docs/61](docs/61-hexdump.md) |
| 62| `wc` + FAT cycle-guard (review fix)    | [docs/62](docs/62-wc-and-fatguard.md) |
| 63| `cal` calendar (RTC + day-of-week)     | [docs/63](docs/63-cal.md) |
| 64| `grep` (search file contents)          | [docs/64](docs/64-grep.md) |
| 65| Graphical System Monitor (live bars)   | [docs/65](docs/65-system-monitor.md) |
| 66| SHA-256 + `sha256` checksum            | [docs/66](docs/66-sha256.md) |
| 67| Customizable browser bookmarks (SITES) | [docs/67](docs/67-bookmarks.md) |
| 68| AES-128 + `crypt` file encryption      | [docs/68](docs/68-aes-crypt.md) |
| 69| `base64` encoding                      | [docs/69](docs/69-base64.md) |
| 70| Review-driven syscall/crypto hardening | [docs/70](docs/70-review-hardening.md) |
| 71| HTTP chunked transfer-encoding decode  | [docs/71](docs/71-chunked-http.md) |
| 72| Keyboard link navigation (Tab/n/p/Enter)| [docs/72](docs/72-keyboard-link-nav.md) |
| 73| `wget` — download from web to disk     | [docs/73](docs/73-wget.md) |
| 74| 8th review: browser hardening          | [docs/74](docs/74-review-browser-hardening.md) |
| 75| Keyboard window switching (F2)          | [docs/75](docs/75-window-cycle.md) |
| 76| Window maximize/restore (F4)            | [docs/76](docs/76-maximize.md) |
| 77| Bookmark current page (a) + Back fix    | [docs/77](docs/77-bookmark-add.md) |
| 78| `<pre>` preformatted text + clip fix    | [docs/78](docs/78-pre-rendering.md) |
| 79| 9th review: Back URL race fix           | [docs/79](docs/79-review-back-race.md) |
| 80| Numbered & nested list rendering        | [docs/80](docs/80-lists.md) |
| 81| Basic table rendering (pipe-separated)  | [docs/81](docs/81-tables.md) |
| 82| Interactive Files window (keyboard)     | [docs/82](docs/82-file-browser.md) |
| 83| 10th review: WM gesture fixes           | [docs/83](docs/83-review-wm.md) |
| 84| HTML entity decoding (hex/typographic)  | [docs/84](docs/84-entities.md) |
| 85| Run an ELF program from disk            | [docs/85](docs/85-run-from-disk.md) |
| 86| Terminal scrollback (PgUp/PgDn)         | [docs/86](docs/86-scrollback.md) |
| 87| Editor keeps the cursor visible         | [docs/87](docs/87-editor-scroll.md) |
| 88| In-page find (\\ + query)               | [docs/88](docs/88-find.md) |
| 89| `<img>` alt text rendering              | [docs/89](docs/89-img-alt.md) |
| 90| Find: all matches + prev/next           | [docs/90](docs/90-find-all.md) |
| 91| Form `<input>` placeholders             | [docs/91](docs/91-form-inputs.md) |
| 92| PNG image rendering (DEFLATE + PNG)     | [docs/92](docs/92-png-images.md) |
| 93| Conway's Game of Life (7th app)         | [docs/93](docs/93-game-of-life.md) |
| 94| Bigger fetch buffers (real-sized images)| [docs/94](docs/94-bigger-buffers.md) |
| 95| PNG palette support (colour type 3)     | [docs/95](docs/95-png-palette.md) |
| 96| Clickable inline images                 | [docs/96](docs/96-clickable-images.md) |
| 97| GIF image rendering (LZW)               | [docs/97](docs/97-gif-images.md) |
| 98| Text colour (`<font color>`)            | [docs/98](docs/98-font-color.md) |
| 99| Tetris (8th app) — 100 milestones!      | [docs/99](docs/99-tetris.md) |
| 100| Browser view-source (`u`)              | [docs/100](docs/100-view-source.md) |
| 101| Breakout (9th app)                      | [docs/101](docs/101-breakout.md) |
| 102| Window tiling (F5/F6)                   | [docs/102](docs/102-window-tiling.md) |
| 103| Browser nav keys (home, bottom)         | [docs/103](docs/103-browser-nav-keys.md) |
| 104| Multi-cluster root dir (lift 16-file cap) + Files scroll | [docs/104](docs/104-multicluster-root.md) |
| 105| Compositor opts: partial cursor-rect blit (~1700× less/move) + memcpy flush | [docs/105](docs/105-cursor-blit-optimization.md) |
| 106| Word-at-a-time `memcpy`/`memset` (96K-case host-verified)    | [docs/106](docs/106-fast-memcpy-memset.md) |
| 107| Clip-once `fb_fill_rect` (hottest draw primitive)           | [docs/107](docs/107-fast-fill-rect.md) |
| 108| Fast glyph blit (`fb_glyph`/`fb_glyph_fg` in-bounds path)   | [docs/108](docs/108-fast-glyph-blit.md) |
| 109| Shell `history` command (exposes the command-recall ring)   | [docs/109](docs/109-shell-history.md) |
| 110| Browser inline `<code>`/`<tt>`/`<kbd>`/`<samp>` styling      | [docs/110](docs/110-inline-code.md) |
| 111| **Inline images** in the browser (local PNG/GIF in the flow) | [docs/111](docs/111-inline-images.md) |
| 112| **From-scratch baseline JPEG decoder** (integer IDCT, fuzzed) | [docs/112](docs/112-jpeg-decoder.md) |
| 113| `<img width/height>` sizing for inline images               | [docs/113](docs/113-img-dimensions.md) |
| 114| Adam7 interlaced PNG (completes PNG support)                | [docs/114](docs/114-adam7-png.md) |
| 115| Tab completion of filenames in the shell line editor       | [docs/115](docs/115-tab-completion.md) |
| 116| **Progressive JPEG** (multi-scan, AC refinement; completes JPEG) | [docs/116](docs/116-progressive-jpeg.md) |
| 117| **Animated GIF** (multi-frame decode + timer-driven playback)    | [docs/117](docs/117-animated-gif.md) |
| 118| Minesweeper (6th game) + start page links the local demos       | [docs/118](docs/118-minesweeper.md) |
| 119| X25519 (Curve25519 ECDH) — first step toward HTTPS/TLS          | [docs/119](docs/119-x25519.md) |
| 120| AES-128-GCM (the TLS AEAD; matches OpenSSL over 4000 cases)     | [docs/120](docs/120-aes-gcm.md) |
| 121| HMAC-SHA256 + HKDF + HKDF-Expand-Label (TLS key schedule)       | [docs/121](docs/121-hmac-hkdf.md) |
| 122| ChaCha20-Poly1305 AEAD (RFC 8439 + OpenSSL) — TLS crypto complete | [docs/122](docs/122-chacha20-poly1305.md) |
| 123| X.509/ASN.1 cert parser (pubkey extract; vs OpenSSL, fuzzed) | [docs/123](docs/123-x509-parser.md) |
| 124| Bignum + RSA PKCS#1 & PSS signature verify (vs Python + OpenSSL) | [docs/124](docs/124-bignum-rsa.md) |
| 125| ECDSA-P256 signature verify — TLS crypto+cert toolkit complete | [docs/125](docs/125-ecdsa-p256.md) |
| 126| Reusable TCP stream API (foundation for HTTPS)               | [docs/126](docs/126-tcp-stream-api.md) |
| 127| **TLS 1.3 client — the browser fetches real HTTPS pages**    | [docs/127](docs/127-tls-https.md) |
| 128| HTML entities in `alt` text + Latin-1 folding (HTTPS-page fidelity) | [docs/128](docs/128-entity-alt-decoding.md) |
| 129| UTF-8 body text decoding (folds to ASCII) — the real web speaks UTF-8 | [docs/129](docs/129-utf8-text.md) |
| 130| **Click-through HTTPS browsing** (relative-link scheme + multi-fetch fix) | [docs/130](docs/130-https-link-following.md) |
| 131| HTTPS from the shell — `get`/`wget` over TLS (`SYS_https`)       | [docs/131](docs/131-shell-https.md) |
| 132| DNS + ARP caches — faster multi-page browsing on a site         | [docs/132](docs/132-dns-cache.md) |
| 133| Parser robustness: HTML comments (`<!-- … -->`) + inline `<svg>` suppression | [docs/133](docs/133-html-comments.md) |
| 134| Window minimize / restore (F3) — desktop window management      | [docs/134](docs/134-window-minimize.md) |
| 135| TCP partial-segment fix + real-web validation (renders live NPR) | [docs/135](docs/135-tcp-partial-segment.md) |
| 136| Network info (IP + gateway) in the System Monitor dashboard     | [docs/136](docs/136-monitor-network.md) |
| 137| Keyboard-drivable Apps menu (F9) — the DE is fully keyboard-usable | [docs/137](docs/137-keyboard-apps-menu.md) |
| 138| **TLS CertificateVerify** — validates real certs' key-possession (vs Cloudflare/NPR/Let's Encrypt) | [docs/138](docs/138-cert-verify.md) |
| 139| X.509 chain-internal verification — each cert signed by the next (real chains) | [docs/139](docs/139-cert-chain.md) |
| 140| **Root-CA trust store** — anchors the chain to a trusted root (NPR → ISRG Root X1) | [docs/140](docs/140-root-ca-anchor.md) |
| 141| SHA-384/512 + SHA-384 cert signatures (microsoft.com chain 2/2 verified) | [docs/141](docs/141-sha384.md) |
| 142| **ECDSA P-384** — full EC chain verification (example.com chain 3/3)    | [docs/142](docs/142-ecdsa-p384.md) |
| 143| Expanded trust store + key-match anchoring (example.com fully validated to root) | [docs/143](docs/143-trust-store-expand.md) |
| 144| **From-scratch JavaScript interpreter** — lexer + parser + tree-walking evaluator; shell `js` / `js file.js` | [docs/144](docs/144-js-interpreter.md) |
| 145| **JavaScript runs in the browser** — pages execute their `<script>` tags (document.write) | [docs/145](docs/145-js-in-browser.md) |
| 146| **JS standard library** — Math, JSON.stringify, Object.keys, String/Array methods (map/filter/…) | [docs/146](docs/146-js-stdlib.md) |
| 147| **Arrow functions** — `x => x*x`, `(a,b) => a+b`, block bodies, currying | [docs/147](docs/147-arrow-functions.md) |
| 148| **JSON.parse** — recursive-descent parser; completes the JSON round-trip | [docs/148](docs/148-json-parse.md) |
| 149| **Template literals** — `` `hi ${name}, ${1+2}` ``, nesting + escapes | [docs/149](docs/149-template-literals.md) |
| 150| **More Array/String methods** — reduce/find/some/every/findIndex, padStart/padEnd | [docs/146](docs/146-js-stdlib.md) |
| 151| **Clickable JavaScript** — `<a href="javascript:...">` runs JS on click; page updates | [docs/151](docs/151-clickable-js.md) |
| 152| **JS switch / do-while** — full fall-through, default, string cases | [docs/152](docs/152-switch-dowhile.md) |
| 153| **JS for-of** — iterate arrays & string characters | [docs/152](docs/152-switch-dowhile.md) |
| 154| **JS try/catch/finally/throw** — full exceptions; built-in errors catchable | [docs/154](docs/154-try-catch.md) |
| 155| **JS Object.values/entries, Array.isArray/from** — stdlib roundout | [docs/146](docs/146-js-stdlib.md) |
| 156| **JS object shorthand + default params** — `{x,y}`, `function f(a,b=10)` | [docs/146](docs/146-js-stdlib.md) |
| 157| **Persistent page state (localStorage)** — interactive pages that remember (a real counter) | [docs/155](docs/155-localstorage.md) |
| 158| **JS Array.includes/concat, String.lastIndexOf** + a committed regression suite (`make jstest`) | [tests/](tests/) |
| 159| **JS Array.sort** — default string order + custom comparator | [tests/](tests/) |
| 160| **JS Date()** — wall-clock timestamps from the CMOS RTC (shell + in-page) | [kernel/js.c](kernel/js.c) |
| 161| **JS for-in** — iterate object keys / array indices | [tests/](tests/) |
| 162| **JS parseInt(radix)/0x, Array.fill/lastIndexOf** | [tests/](tests/) |
| 163| **JS Object.assign, Array.flat, Math.sign** | [tests/](tests/) |
| 164| **JS Number.toString(radix)** — `(255).toString(16)`→`ff` | [tests/](tests/) |
| 165| **JS `this` / `new` / OOP** — constructors, method shorthand, lexical arrow `this`, `o.n++` | [docs/165](docs/165-this-new-oop.md) |
| 166| **JS `class` / `extends`** — methods, inheritance, override (method-copy model) | [docs/166](docs/166-class-syntax.md) |
| 167| **JS `super`** — super-constructor + `super.method()`, multi-level chains | [docs/167](docs/167-super.md) |
| 168| **JS spread / rest `...`** — `[...a]`, `f(...args)`, `{...o}`, `(...rest)` | [docs/168](docs/168-spread-rest.md) |
| 169| **JS destructuring** — `var [a,...r]=arr`, `{x:y=1,...rest}=o`, nested, for-of | [docs/169](docs/169-destructuring.md) |
| 170| **JS hardening** — `bind_pattern` depth guard (review L1) | [kernel/js.c](kernel/js.c) |
| 171| **JS parameter destructuring** — `function f({a,b})`, `arr.map(([k,v])=>…)` | [docs/171](docs/171-param-destructuring.md) |
| 172| **JS assignment destructuring** — `[a,b]=[b,a]` swap, `({x}=o)`, `[o.p]=…` | [docs/172](docs/172-assignment-destructuring.md) |
| 173| **JS `??` / `?.`** — nullish coalescing + optional chaining (+ arena 1→2 MB) | [docs/173](docs/173-nullish-optional.md) |
| 174| **JS `Map` / `Set`** — ES6 collections, any key type, iteration, `for-of` | [docs/174](docs/174-map-set.md) |
| 175| **JS `?.` full-chain short-circuit** — `a?.b.c()` → `undefined` (review fix) | [kernel/js.c](kernel/js.c) |
| 176| **JS computed property keys** — `{[expr]: v}` | [kernel/js.c](kernel/js.c) |
| 177| **JS stdlib batch** — Array `at`/`flatMap`/`findLast`, String `at`/`replaceAll`, `Object.fromEntries` | [kernel/js.c](kernel/js.c) |
| 178| **JS regular expressions** — from-scratch `RegExp` (test/exec) + String match/replace/split/search; ReDoS-safe | [docs/178](docs/178-regex.md) |

## Roadmap

**Foundation — complete:**
- [x] 0. Boot to 64-bit long mode, print to screen + serial
- [x] 1. Terminal driver (scrolling VGA text + formatted `kprintf`)
- [x] 2. Interrupts: GDT/TSS, IDT, CPU exceptions, PIC remap
- [x] 3. Timer (PIT) + PS/2 keyboard input
- [x] 4. Physical memory manager (multiboot memory map, frame allocator)
- [x] 5. Virtual memory: 4-level paging, higher-half direct map (see docs/05)
- [x] 6. Kernel heap (`kmalloc`/`kfree`)
- [x] 7. Multitasking: kernel threads + round-robin scheduler
- [x] 8. Userspace: ring 3, syscalls, ELF program loader
- [x] 9. A libc + a basic shell
- [x] 10. VFS + FAT32 filesystem + ATA driver + shell `ls`/`cat`

**Hardware + extras:**
- [x] 11. Preemptive scheduling (timer-driven context switches)
- [x] 12. PCI bus enumeration
- [x] 13. e1000 NIC driver + ARP + ICMP ping
- [x] 14. Framebuffer graphics + 8×8 font

**Desktop environment:**
- [x] 15. Graphical console + real 8×16 font
- [x] 16. PS/2 mouse + cursor
- [x] 17. Window manager / compositor (drag, focus, z-order)
- [x] 18. Desktop apps: terminal, file browser, taskbar clock + button
- [x] 19. USB (UHCI) + usb-tablet absolute pointer (cursor tracks 1:1)
- [x] 20. Resizable windows + start menu + Clock/About apps
- [x] 21. Per-process address spaces (real memory isolation)
- [x] 22. Userspace apps as windows (the ring-3 shell, in a window)

**System polish & apps:**
- [x] 23. Sleep/wake (blocking) — the CPU idles instead of busy-spinning
- [x] 24. Visual polish: gradients, shadows, rounded windows, themed taskbar
- [x] 25. Real-time clock (RTC) — real time in the taskbar + `date`
- [x] 26. PC speaker sound + `beep` + a boot chime
- [x] 27. System commands: `mem`, `clear`, `reboot`
- [x] 28. FAT32 **write** — create/save files on disk
- [x] 29. Networking from the shell: `ping` + DNS `resolve`
- [x] 30. A userspace **text editor** (`edit`)
- [x] 31. File delete (`rm`) — full file toolkit: ls/cat/edit/write/rm
- [x] 32. Process spawning + multiple programs (`run`, a 2nd live "clock" app)
- [x] 33. **TCP + HTTP GET** — fetches real web pages over the internet (`get`)
- [x] 34. **Graphical web browser** — parses + renders live HTML in a window
- [x] 35. **Clickable links** — follow links + resolve relative/absolute URLs
- [x] 36. **Non-blocking page loads** — fetch on a worker task, desktop stays live
- [x] 37. **Richer rendering** — page title in the bar, list/definition layout, `<hr>`
- [x] 38. **Browser history / Back** — back stack, `<` button + Backspace
- [x] 39. **`browse <url>`** — open the browser from the shell
- [x] 40. **Concurrency hardening** — atomic WM↔worker hand-offs (review-driven)
- [x] 41. **Browser start page** — built-in bookmarks home, rendered locally
- [x] 42. **Save a page to disk** — browser → FAT32 → shell reads it back
- [x] 43. **FAT32 subdirectories** — `mkdir`/`cd`/`pwd` + path resolution
- [x] 44. **Calculator app** — a third interactive userspace program
- [x] 45. **Browser opens local files** — `file:…` reads the disk (offline reading)
- [x] 46. **FS write hardening** — overwrite no longer duplicates/leaks (review fix)
- [x] 47. **`cp` / `mv`** — file toolkit complete (ls cat edit write rm cp mv mkdir cd pwd)
- [x] 48. **`tree`** — recursive directory listing (first recursive FS op)
- [x] 49. **Taskbar window list** — a chip per window, click to focus
- [x] 50. **`ps`** — list running tasks (id / state / name) from the run queue
- [x] 51. **Bold & italic** — inline emphasis in the browser
- [x] 52. **Compositor scene-cache** — mouse moves no longer re-render the desktop
- [x] 53. **Arrow keys** — extended-scancode decoding (browser scroll, more to come)
- [x] 54. **Command history** — ↑/↓ recall previous commands (responsive typing)
- [x] 55. **Snake** — a real-time game (non-blocking input, prompt repaint)
- [x] 56. **`df`** + review fixes (history erase clamp, lost-wakeup)
- [x] 57. **Text editor** — full-screen editor with cursor, saves to FAT32
- [x] 58. **`find`** — recursive filesystem search
- [x] 59. **HTTP redirects** — the browser follows 3xx `Location:` hops
- [x] 60. **2048** — a second arrow-key game (six userspace apps now)
- [x] 61. **`hexdump`** — inspect a file's raw bytes
- [x] 62. **`wc`** + FAT chain cycle-guard (review fix — no hang on corrupt FAT)
- [x] 63. **`cal`** — month calendar from the RTC
- [x] 64. **`grep`** — search inside files (companion to `find`)
- [x] 65. **System Monitor** — a graphical app with live memory/task bars
- [x] 66. **SHA-256** — verified hash + `sha256` checksums (first step toward TLS)
- [x] 67. **Bookmarks** — customize the browser start page from a `SITES` file
- [x] 68. **AES-128 + `crypt`** — verified cipher, passphrase file encryption
- [x] 69. **`base64`** — encode files to text (rounds out crypto/encoding tools)

**Where next (see WHATS-NEXT.md):** TLS/HTTPS (the big one), `fork`/`exec`,
window minimize, and more apps.
