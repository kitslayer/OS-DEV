# Tests

Host-side regression + fuzz tests for the from-scratch parsers that run
**kernel-side on untrusted input** (a malicious web server, an on-path attacker,
or page scripts) on a **16 KB stack with no guard page** — where an out-of-bounds
access is silent kernel memory corruption. Each suite compiles the *real* kernel
source on the host under **ASan + UBSan** and exercises it with crafted edge
cases + deterministic fuzzing.

## Running

```sh
make check       # run all 22 suites (20 host + 2 in-guest; ~60s total)
make jstest      # JS engine      — tests/js/suite.js vs the golden output
make imgtest     # image decoders — tests/img/img_test.c   (jpeg/png/gif/bmp/inflate)
make x509test    # X.509 parser   — tests/x509/x509_test.c
make nettest     # TCP/IP stack   — tests/net/net_test.c   (packet parse + reassembly)
make fstest      # FAT32 driver   — tests/fs/fs_test.c     (corrupt/cyclic on-disk structures + write stress)
make kattest     # crypto KAT     — tests/crypto/crypto_test.c (RFC/FIPS known-answer vectors)
make svgtest     # SVG rasterizer — tests/svg/svg_test.c   (shapes/paths + adversarial XML fuzz)
make deflatetest # DEFLATE/gzip   — compressor vs the decoder, round-trip
make pngenctest  # PNG encoder    — encoder vs the decoder, round-trip
make ziptest     # ZIP extractor  — extraction + corrupt-input fuzz
make tartest     # tar extractor  — extraction + corrupt-input fuzz
make heaptest    # userspace malloc — umalloc.c alloc/free regression
make wavtest     # WAV header     — wav_parse over corrupt RIFF chunks
make elftest     # ELF64 loader   — tests/elf/elf_test.c   (validators + load round-trip + every shipped app binary)
make httptest    # HTTP parsers   — tests/http/http_test.c (chunked-transfer decode + header scans)
make kheaptest   # kernel heap    — tests/kheap/kheap_test.c (kmalloc/kfree split/coalesce/grow torture)
make jsonfuzztest # JSON.parse     — tests/jsonfuzz (untrusted/malformed/deep server JSON)
make regexfuzztest # regex engine  — tests/regexfuzz (ReDoS shapes + malformed patterns, compile+search)
make jssrcfuzztest # JS source     — tests/jssrcfuzz (full parse+run pipeline on adversarial script source)
make htmlentfuzztest # HTML entities — tests/htmlentfuzz (decode_entity over untrusted/malformed page bytes)
make boottest    # in-guest boot  — boots the real kernel headless, asserts every bring-up marker (no crash)
make gfxtest     # in-guest gfx   — captures the VGA framebuffer, asserts the desktop actually painted
```

The last two boot the real kernel under QEMU (unlike the host suites, which
`#include` one `.c` in isolation). They **SKIP cleanly** if QEMU — or, for
`gfxtest`, `socat`/`python3` — is unavailable, so the host-only gate still
passes. `make test` is a related target — the same headless boot but printing
the COM1 log for a human to read, rather than asserting markers.

For *interactive* headless debugging, `tools/osdrive.py` drives the booted
desktop — inject keys (HMP `sendkey`) and absolute-mouse clicks/drags (QMP
`input-send-event`), and grab framebuffer screenshots — e.g.
`tools/osdrive.py --out /tmp -c 'key f9; key ret; sleep 2; shot browser.png'`.
This is how the M558–M561 desktop changes were verified without a display.

You can also run the JS suite inside the OS: `js suite.js` (if copied onto the
disk), or the baked-in demos `js`, `js showcase.js`, `js sample.js`.

## What each suite covers

| Suite | Source under test | What it checks |
|-------|-------------------|----------------|
| `jstest`  | `kernel/js.c` | A golden-output regression of ~60 language/stdlib features (core operators, closures/recursion, arrows, default params, arrays + higher-order methods, strings, objects, `JSON`, template literals, `switch`/`for-of`/`do-while`, `try/catch/finally`); ASan/UBSan-clean, ran-to-completion, matches `tests/js/suite.expected`. Includes the M419 `instanceof` + M420 `+` ToPrimitive fixes, the integer-arithmetic / div-by-zero-guard contract, and the ToPrimitive/ToNumber coercion edges (`[]+[]`, `+[]`, `null+1`, `true+true`, string arithmetic). |
| `imgtest` | `jpeg.c` `png.c` `gif.c` `inflate.c` | The M422 JPEG DRI out-of-bounds-read PoC, truncated/bare-magic headers, a 120k-iteration random-bytes fuzz through all three decoders, and a direct 120k DEFLATE fuzz (huffman + LZ77). |
| `x509test`| `kernel/x509.c` | Crafted adversarial DER (4 GB length claim, truncated/indefinite-length, nested headers) + a 200k-iteration fuzz of the `tlv` reader. **Plus SAN/hostname verification (M451):** 4 real openssl-generated certs assert that `find_san` parses subjectAltName dNSNames and `host_matches_cert` applies the RFC 6125 rules — exact + multi-SAN match, case-insensitivity, single-leftmost-label wildcard (matches `a.x.com`, rejects the bare apex `x.com` and `a.b.x.com` and `*.com`), CN fallback only when there is no SAN, and **no** CN fallback when a SAN is present — plus a 400k-iteration mutation fuzz of those certs (byte-flips + truncation) driving `find_san` over near-valid SAN/extension DER, asserting `n_san` stays in `[0,16]` and the matcher never OOBs. |
| `nettest` | `kernel/net.c` | 150k random Ethernet/IPv4/TCP frames through `tcp_recv_seg`, and 150k crafted `seq`/`dlen` through the 96 KB `ooo_store` reassembly buffer (far-future/past/wraparound). Stubs the NIC + timer. |
| `fstest`  | `kernel/fat32.c` | **Read path:** a valid minimal FAT32 image then 12k corrupted copies (BPB/FAT/root-dir bytes randomized) through `mount`/`list`/`read`/`find`/`tree` — locks the M435 `cluster_in_range` guard, the cluster-chain cycle guard, and the dir-recursion depth caps (a corrupt/cyclic FAT must never OOB or hang). **Write path:** 8k accumulating `write`/`delete`/`mkdir` ops (the "heavy repeated writes" scenario) — confirms `alloc_cluster`/`add_entry`/`write_fat`/chain-extension are memory-safe (its known fragility is logical/persistence, not OOB). `#include`s fat32.c and stubs the disk (`ata_read`/`ata_write` → an in-memory image) + `vfs_register`. |
| `kattest`  | `sha256.c` `sha512.c` `aes.c` `aesgcm.c` `chachapoly.c` `hkdf.c` `x25519.c` | **Known-answer** (not fuzz): checks each from-scratch crypto primitive against its published vector — SHA-256/384/512 (FIPS 180-4), HMAC-SHA256 (RFC 4231), HKDF (RFC 5869), AES-128 block (FIPS-197), AES-128-GCM (GCM spec TC1/2), ChaCha20-Poly1305 (RFC 8439 §2.8.2, incl. forged-tag rejection), X25519 (RFC 7748 §5.2/6.1), and **signature verify** — ECDSA P-256/P-384 + RSA-2048 PKCS#1 (vectors openssl-generated + independently re-verified; valid sig accepted, tampered sig rejected), covering the X.509 cert-path-validation crypto. The crypto .c files are compiled as separate translation units (no static collisions); mem* resolves to libc. Locks the TLS 1.3 crypto foundation against silent regression. |
| `deflatetest` | `kernel/deflate.c` `kernel/inflate.c` | Round-trips the from-scratch DEFLATE/gzip compressor through the decoder (literal/fixed/dynamic-Huffman + LZ77 matches) and confirms byte-exact recovery; ASan/UBSan-clean. |
| `pngenctest` | `kernel/png_encode.c` `kernel/png.c` | Round-trips the PNG encoder through the decoder over assorted dimensions/patterns (filters + zlib wrapping), asserting pixel-exact recovery; ASan/UBSan-clean. |
| `ziptest` | `kernel/zip.c` `kernel/inflate.c` | Exact extraction of a multi-entry ZIP (stored + deflated, subdirs, empty/large files) vs the originals, plus a system-`zip`-built archive, plus fuzz: all truncated prefixes, single-byte corruptions, 80k multi-byte mutations, and random buffers must extract cleanly or fail without OOB. |
| `tartest` | `kernel/tar.c` | Exact extraction of a ustar archive vs the originals, plus truncation + single-byte-corruption + 20k random-buffer fuzz; corrupt input must never OOB. |
| `heaptest` | `user/umalloc.c` | Regression of the userspace first-fit allocator (malloc/free/calloc/realloc over an sbrk-backed arena): split/coalesce/reuse correctness, ASan/UBSan-clean. |
| `wavtest` | `kernel/wav.c` | The RIFF/WAVE header parser `wav_parse` (walks untrusted chunk data): valid mono/stereo headers parse correctly, plus a fuzz pass over truncated/corrupt RIFF chunks that must never OOB. |
| `elftest` | `kernel/elf.c` | The ELF64 loader (the ring-3 trust boundary). A known-good minimal ELF round-trips through `elf_load` (correct entry, file bytes copied to `p_vaddr`, `.bss` tail zeroed, via an mmap-backed guest-memory stub); the pure validators (`elf_check_header`/`elf_check_phdr`) are fuzzed over every truncated prefix, every single-byte header corruption, and 200k random buffers so a malformed ELF can never OOB-read or be accepted with a segment escaping the image or the user range; and **every shipped app binary** (all 29 — shell, the games, DOOM, Quake) is loaded through `elf_load` to guard against a linker/toolchain regression. |
| `httptest` | `kernel/http.c` | The HTTP/1.x response parsers that read untrusted server/CDN bytes. Regression: chunked bodies, hex/large chunk sizes, chunk extensions, truncation, case-insensitive header scans, and `Location:` extraction with buffer-truncation all produce the expected results. Fuzz: every truncated prefix, every single-byte corruption, and 400k random buffers — in-place de-chunking (which memmoves with attacker-controlled hex sizes) never OOBs or returns a length outside the input, and `find_loc` never overruns its output. |
| `kheaptest` | `kernel/kheap.c` | The kernel heap `kmalloc`/`kfree`/`kzalloc` (underlies every kernel allocation), run against a real mmap'd arena. 400k random alloc/free ops with a per-block byte pattern re-verified each pass (catches any overlap/corruption), `kzalloc` zeroing, repeated `grow_heap()`, and a free-list walk asserting the blocks tile `[base, heap_end)` exactly with no gaps or cycles; ASan/UBSan-clean. |
| `jsonfuzztest` | `kernel/js.c` (`nat_json_parse`) | The engine's `JSON.parse` parses untrusted server/API JSON in-kernel. Drives the parser directly (js.c #included with `JS_NO_MAIN`) over every truncated prefix + single-byte corruption of a rich document, 300k random punctuation-biased buffers, and 1..5000-deep bracket nesting (must hit the depth guard, not overflow); valid documents confirm correct parsing. Exactly-sized buffers so any over-read red-zones. |
| `regexfuzztest` | `kernel/js.c` (`re_compile`/`re_search`) | The regex engine compiles untrusted patterns and runs them on untrusted strings (historically the source of two critical matcher stack-overflows). ReDoS shapes (`(a+)+$`, `(a*)*`, deep groups, huge `{n,m}`, unterminated classes) against long `a` runs + 200k random pattern/subject pairs; the step-budget + depth guard must keep every run bounded with no OOB/overflow/hang. |
| `jssrcfuzztest` | `kernel/js.c` (`js_run_doc`) | The full parse+run pipeline on untrusted SOURCE — the browser's `<script>`/`javascript:`. Truncations + sampled corruptions of a rich script, 200k random token-biased buffers, and 1..4000-deep nesting through the lexer/parser (MAXDEPTH guard) and the loop/recursion/arena run guards; adversarial source must fail gracefully, never OOB/overflow/hang. |
| `htmlentfuzztest` | `kernel/htmlentity.c` (`decode_entity`) | The HTML character-reference decoder reads untrusted page bytes (`&amp;`, `&#NNN;`, `&#xHH;`, named). Known entities decode correctly; then every truncated prefix + single-byte corruption of a battery (incl. huge/overflowing numeric refs and bare `&`/`&#`/`&#x`) + 300k random entity-char-biased buffers, in exactly-sized buffers so any over-read red-zones. |
| `svgtest`  | `kernel/svg.c` | The from-scratch integer-only SVG rasterizer (parses untrusted web XML in-kernel). 8 unit cases that must render correctly (rect, viewBox scaling, circle + cubic-bezier path, stroked polygon, named colors, **affine transforms** — `<g>`-group + per-shape `translate`/`scale`/`rotate`/`matrix`, nested-group composition, and the CTM correctly restored after `</g>` — **paint inheritance** — `fill`/`stroke` inherited from the root `<svg>`/enclosing `<g>`, per-shape override, the `inherit` keyword, and inherited paint restored after `</g>` — **and opacity** — `fill-opacity`/`opacity` per shape, group `<g opacity>` inherited, group×element compounding, and `in_alpha` restored after `</g>` — **and gradients** — a linear red→blue across the box + a radial white→black centre→edge, exercising the `<defs>` pre-pass, `fill=url(#id)` resolution and per-pixel evaluation) plus ~520k in-suite fuzz iterations: 100k random bytes, 100k mutations of valid SVG, 320k structured (random shape/path/attr/**transform**/**fill**/**opacity**/**gradient** trees), and adversarial inputs (deep nesting, huge coordinate counts, a huge-coordinate gradient that would overflow the projection's int64 intermediate if unclamped, truncation) — plus a separate **6M-iteration gradient-focused fuzz** run during review. Locks bounds-safety on the scanline-fill crossings buffer, the per-shape point list in caller scratch, the `<g>` transform + paint + opacity stacks, the gradient table/stop caps + the `grad_color_at` fixed-point, and `parse_num` against the UB bugs the author fuzz first caught (negative shifts, `num<<16` int64 overflow). |
| `boottest` | the **whole kernel + driver stack** (`tests/run-boot-tests.sh`) | Unlike every row above (which `#include`s one `.c` in isolation), this boots the *real* `build/kernel32.elf` headless under QEMU, captures COM1, and asserts all 9 required bring-up markers print in order with no crash: core bring-up (PMM/VMM/IDT), the preemptive scheduler, per-process address-space isolation, PCI enumeration, the e1000+ARP+ICMP stack (ping to the SLIRP gateway), FAT32 mount, AC'97 audio bring-up, USB UHCI+tablet, and reaching `desktop_run()`; and that no `panic`/`unhandled exception`/`page fault`/`#GP` appears anywhere. Two **internet-dependent** checks are informational (non-fatal — offline hosts stay green): a real HTTP GET to live example.com, and — exercising the whole **from-scratch TLS 1.3 stack** end to end — a real HTTPS GET whose `certverify=ok` confirms X25519 ECDHE + AEAD + X.509 chain validation to a trusted root + ECDSA/RSA `CertificateVerify`. Re-established now that QEMU runs again (a SIGSTKFLT launch failure had blocked in-guest verification for many milestones). |
| `gfxtest`  | the **compositor / framebuffer / font stack** (`desktop.c`/`fb.c`/`fbcon.c`/`font.c`/`vga.c`) | The serial boot above proves the kernel *reached* the desktop, not that anything *painted*. This boots headless, waits for the desktop hand-off, captures the emulated VGA framebuffer via the QEMU monitor's `screendump` (HMP over a unix socket driven by `socat`), and asserts the PPM looks like a real painted desktop: 1024×768 (the desktop mode-set ran, not the 640×480 console), ≥40 distinct colors, and no single color >98% (catches an all-black hang). Retries the screendump to absorb paint-timing jitter. |

## Validated to catch regressions

Each fuzz harness is **verified to fail** when its guard is removed:

- `imgtest` aborts (ASan) if the JPEG DRI `seglen >= 2` guard is removed.
- `x509test` aborts if `tlv`'s `len > end-p` bound is removed.
- `nettest` aborts if `ooo_store`'s `off > OOO_CAP - dlen` bound is removed.
- `fstest` aborts (ASan stack-overflow) if fat32's dir-recursion depth caps are removed (a cyclic directory recurses unbounded).
- `kattest` is itself the regression check — any change to a primitive's output fails the byte-exact vector comparison; the AEAD forged-tag case also verifies the reject path.
- `jstest` diffs against the golden, so any output change fails.
- `svgtest` aborts (ASan/UBSan) if a bounds guard is removed (e.g. the scanline crossings cap or the point-list cap) and fails loudly if a unit case stops rendering its expected pixels; UBSan also re-catches the original `parse_num` negative-shift/overflow if reintroduced.
- `elftest` fails loudly if `elf_check_header`'s program-header-table bound is removed (a phdr read then runs past the image — the harness flags the broken in-bounds promise).
- `httptest` aborts (ASan stack-buffer-overflow) if `http_dechunk`'s `sz > room` truncation clamp is removed (the in-place memmove then runs past the body).
- `kheaptest` aborts (per-block pattern mismatch) if `kmalloc` stops marking a block used (`b->free = 0`) — the same block is handed out twice and the live allocations overlap.
- `jsonfuzztest` aborts (ASan heap-buffer-overflow at `jp_string`) if the string scanner's `jp_end` bound is removed (an unterminated string over-reads).
- `regexfuzztest` aborts (ASan stack-overflow in `re_run`) if the matcher's `depth>900` guard is removed (a pathological pattern recurses unbounded).
- `jssrcfuzztest` exercises the same ASan red-zone + termination-guard machinery proven by the two above on the full lexer/parser/eval pipeline.
- `htmlentfuzztest` aborts (ASan buffer-overflow in `decode_entity`) if its `n < maxlen` scan bound is removed (an entity with no `;` then reads past the input).
- `boottest` fails (lists the missing marker, exit 1) if any bring-up step stops printing its marker — verified by pointing `QEMU=true` at it (empty boot log → every marker MISSING).
- `gfxtest`'s `ppm_check.py` is verified as a real oracle: an all-black 1024×768 frame → FAIL (1 distinct color), a 640×480 console frame → FAIL (resolution below the desktop mode), a real painted desktop → PASS.

## Not covered here

The symmetric/hash/KDF/DH crypto **and** the RSA/ECDSA signature-verify are now
correctness-locked by `kattest` (above); the `bignum` that backs them is further
exercised via `x509test`'s cert-chain validation and the live handshakes. `tls.c`'s handshake-message parsers are intentionally
*not* fuzzed by a committed harness: a naive random fuzz fast-fails at
`x509_parse` / bails without a seeded leaf key (so it exercises nothing), and a
meaningful one needs valid-DER + valid-key seeds — disproportionate for code
that the manual security review covered thoroughly and whose cert DER parser is
already deeply fuzzed by `x509test`. The handshake path *is* now exercised end
to end at every boot (and informationally by `boottest`): the kernel completes a
real TLS 1.3 HTTPS GET to example.com — full handshake, chain validation to a
trusted root, and `CertificateVerify` — so a regression that breaks the live
handshake is caught even though there's no committed record-layer fuzzer. See
[../docs/422-untrusted-input-security-audit.md](../docs/422-untrusted-input-security-audit.md).
