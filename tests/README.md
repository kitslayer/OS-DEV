# Tests

Host-side regression + fuzz tests for the from-scratch parsers that run
**kernel-side on untrusted input** (a malicious web server, an on-path attacker,
or page scripts) on a **16 KB stack with no guard page** — where an out-of-bounds
access is silent kernel memory corruption. Each suite compiles the *real* kernel
source on the host under **ASan + UBSan** and exercises it with crafted edge
cases + deterministic fuzzing.

## Running

```sh
make check       # run all seven suites (~6s total)
make jstest      # JS engine     — tests/js/suite.js vs the golden output
make imgtest     # image decoders — tests/img/img_test.c  (jpeg/png/gif/inflate)
make x509test    # X.509 parser   — tests/x509/x509_test.c
make nettest     # TCP/IP stack   — tests/net/net_test.c  (packet parse + reassembly)
make fstest      # FAT32 driver   — tests/fs/fs_test.c    (corrupt/cyclic on-disk structures + write stress)
make kattest     # crypto KAT     — tests/crypto/crypto_test.c (RFC/FIPS known-answer vectors)
make svgtest     # SVG rasterizer — tests/svg/svg_test.c    (shapes/paths + adversarial XML fuzz)
```

`make test` is a *different* target — the headless QEMU boot smoke test.

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
| `svgtest`  | `kernel/svg.c` | The from-scratch integer-only SVG rasterizer (parses untrusted web XML in-kernel). 8 unit cases that must render correctly (rect, viewBox scaling, circle + cubic-bezier path, stroked polygon, named colors, **affine transforms** — `<g>`-group + per-shape `translate`/`scale`/`rotate`/`matrix`, nested-group composition, and the CTM correctly restored after `</g>` — **paint inheritance** — `fill`/`stroke` inherited from the root `<svg>`/enclosing `<g>`, per-shape override, the `inherit` keyword, and inherited paint restored after `</g>` — **and opacity** — `fill-opacity`/`opacity` per shape, group `<g opacity>` inherited, group×element compounding, and `in_alpha` restored after `</g>` — **and gradients** — a linear red→blue across the box + a radial white→black centre→edge, exercising the `<defs>` pre-pass, `fill=url(#id)` resolution and per-pixel evaluation) plus ~520k in-suite fuzz iterations: 100k random bytes, 100k mutations of valid SVG, 320k structured (random shape/path/attr/**transform**/**fill**/**opacity**/**gradient** trees), and adversarial inputs (deep nesting, huge coordinate counts, a huge-coordinate gradient that would overflow the projection's int64 intermediate if unclamped, truncation) — plus a separate **6M-iteration gradient-focused fuzz** run during review. Locks bounds-safety on the scanline-fill crossings buffer, the per-shape point list in caller scratch, the `<g>` transform + paint + opacity stacks, the gradient table/stop caps + the `grad_color_at` fixed-point, and `parse_num` against the UB bugs the author fuzz first caught (negative shifts, `num<<16` int64 overflow). |

## Validated to catch regressions

Each fuzz harness is **verified to fail** when its guard is removed:

- `imgtest` aborts (ASan) if the JPEG DRI `seglen >= 2` guard is removed.
- `x509test` aborts if `tlv`'s `len > end-p` bound is removed.
- `nettest` aborts if `ooo_store`'s `off > OOO_CAP - dlen` bound is removed.
- `fstest` aborts (ASan stack-overflow) if fat32's dir-recursion depth caps are removed (a cyclic directory recurses unbounded).
- `kattest` is itself the regression check — any change to a primitive's output fails the byte-exact vector comparison; the AEAD forged-tag case also verifies the reject path.
- `jstest` diffs against the golden, so any output change fails.
- `svgtest` aborts (ASan/UBSan) if a bounds guard is removed (e.g. the scanline crossings cap or the point-list cap) and fails loudly if a unit case stops rendering its expected pixels; UBSan also re-catches the original `parse_num` negative-shift/overflow if reintroduced.

## Not covered here

The symmetric/hash/KDF/DH crypto **and** the RSA/ECDSA signature-verify are now
correctness-locked by `kattest` (above); the `bignum` that backs them is further
exercised via `x509test`'s cert-chain validation and the live handshakes. `tls.c`'s handshake-message parsers are intentionally
*not* fuzzed by a committed harness: a naive random fuzz fast-fails at
`x509_parse` / bails without a seeded leaf key (so it exercises nothing), and a
meaningful one needs valid-DER + valid-key seeds — disproportionate for code
that the manual security review covered thoroughly and whose cert DER parser is
already deeply fuzzed by `x509test`. See
[../docs/422-untrusted-input-security-audit.md](../docs/422-untrusted-input-security-audit.md).
