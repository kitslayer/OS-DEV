# Security & test posture (consolidated reference)

A single map of OS-DEV's untrusted-input trust boundaries, what's been audited,
how it's locked by tests, and the honest known limits. The per-area write-ups
([docs/422](422-untrusted-input-security-audit.md),
[docs/434](434-browser-parser-security-audit.md),
[docs/435](435-fat32-cluster-range-guard.md)) have the detail; this is the index.

## Why it matters here

Every parser below runs **in-kernel**, on a **256 KB stack with no guard page**
(the browser/WM and the fetch worker both run page JS + TLS). An out-of-bounds
read/write isn't a clean fault — it's silent kernel-memory corruption. So the
untrusted-input parsers are the crown-jewel attack surface and get review +
committed fuzz/KAT coverage.

## Trust boundaries (attacker-controlled bytes)

| Surface | Path | Status |
|---|---|---|
| TCP/IP frames | NIC → `net.c` (Ethernet/IPv4/TCP + 96 KB OOO reassembly) | reviewed bounds-safe; **`nettest`** fuzzes it |
| TLS records / handshake | `tls.c` | manual review (bounds-safe); cert DER fuzzed by `x509test`; crypto KAT'd |
| X.509 certificates | `x509.c` `tlv` DER reader | reviewed; **`x509test`** fuzzes (size_t length accumulation defeats the overflow trap) |
| HTML / CSS | `browser.c` (`parse_html`, `decode_entity`, `parse_color`, `sel_parse`, attr/style parsers) | M422 high-level + **M434 granular** review — bounds-safe |
| Images (PNG/GIF/JPEG/DEFLATE) | `png.c`/`gif.c`/`jpeg.c`/`inflate.c` | **`imgtest`** fuzzes; M422 fixed a JPEG DRI OOB-read |
| Page JavaScript | `js.c` (tree-walking, integer, 12 MB arena) | many reviews; **`jstest`** golden; depth/step/OOM guards (256 KB stack) |
| DNS hostname | `net.c` `dns_resolve` query builder | M423 fixed a kernel-stack overflow (>238-char host) |
| Disk (FAT32) | `fat32.c` cluster chains + dir entries | **M435** cluster-range guard + cycle/depth caps; **`fstest`** fuzzes read+write |
| ELF programs | `elf.c` loader | reviewed: every offset/size/vaddr validated vs image + user range (overflow-safe) |

## Real bugs found + fixed (all ASan/UBSan-verified)

- **M422** — JPEG `DRI` marker out-of-bounds read (`seglen >= 2` guard).
- **M423** — DNS query-builder kernel-stack overflow (bound each label to 64).
- **M434** — `decode_entity`'s `s[1]` read was caller-contract-safe; hardened with a `maxlen < 2` guard (defense-in-depth).
- **M435** — FAT32 followed out-of-range clusters (wrapped/garbage reads); `cluster_in_range` ends the chain cleanly.

Everything else reviewed came back bounds-safe (the rest of the browser/CSS
parsers, the TLS/X.509 path, the image decoders, the ELF loader, the FAT32 write
path's memory-safety).

## Committed test coverage — `make check` (24 suites; ~75 s)

Grown well past the original six. The full, current list (with what each locks)
lives in [../tests/README.md](../tests/README.md); the shape now:

- **21 host-side ASan/UBSan suites** — each compiles the *real* kernel `.c` on
  the host and fuzzes it. The original six (below) plus, over the years:
  `svgtest`, `deflatetest`, `pngenctest`, `ziptest`, `tartest`, `wavtest`,
  `heaptest` (userspace malloc), `kheaptest` (kernel heap), `elftest` (the ring-3
  loader + every shipped binary), `httptest` (chunked decode), and the
  untrusted-parser fuzzers `jsonfuzztest` / `regexfuzztest` / `jssrcfuzztest` /
  `htmlentfuzztest` / `htmlattrtest` (the browser's HTML attribute scanners).
- **3 in-guest QEMU suites** (added once QEMU could run again — see WHATS-NEXT):
  `boottest` boots the real kernel headless and asserts all 9 bring-up markers
  with no crash; `gfxtest` captures the VGA framebuffer and asserts the desktop
  painted; `browsertest` launches the Browser from the Apps menu and asserts its
  home page rendered — the end-to-end guard for `parse_html`, which is too
  coupled to fuzz in isolation. The boot also runs a live **TLS 1.3 HTTPS**
  self-test (real example.com) — full handshake + chain-to-root + CertificateVerify.

The original six:

| Suite | Locks |
|---|---|
| `jstest`  | ~60 JS features vs a golden, incl. ToPrimitive/coercion + div-by-zero-guard semantics |
| `imgtest` | jpeg/png/gif/inflate fuzz + the M422 PoC |
| `x509test`| cert DER parser fuzz |
| `nettest` | packet parser + OOO reassembly fuzz |
| `fstest`  | FAT32 read (corrupt/cyclic images) + write (heavy-churn) — locks M435 + the cycle/depth caps |
| `kattest` | crypto vs RFC/FIPS known-answer vectors (SHA-256/384/512, HMAC, HKDF, AES, AES-GCM, ChaCha20-Poly1305 + forged-tag reject, X25519) + signature verify (ECDSA P-256/384, RSA-2048 PKCS#1 — valid accepted, tampered rejected) |

Each fuzz suite is verified to catch a reintroduced bug (e.g. removing fat32's
dir-recursion depth caps trips an ASan stack-overflow; any crypto-output change
fails its vector; the image PoC aborts if the DRI guard is removed; loosening
`find_attr`'s slice bound trips an ASan overflow). `make test` is the same
headless boot as `boottest` but prints the COM1 log for a human instead of
asserting markers.

## Honest known limits (not bugs — documented design choices)

- **Cert validation: hostname + validity are ENFORCED; chain-to-root anchoring
  is reported.** Since M451 the leaf cert's **SAN/CN must name the host** and it
  must be **within its validity period** — a definitive hostname mismatch
  (`host_ok==0`) or an expired/not-yet-valid cert (`cert_time_ok==0`) makes
  `tls_get_inner` *reject the connection before sending the request* (tls.c:595;
  both fail *open* only when genuinely uncertain — a mega-SAN cert we can't fully
  read, or an unset RTC < year 2020 — so the checks can't break all of HTTPS).
  `CertificateVerify` (leaf-key possession) and the chain's internal issuer links
  are verified; anchoring the chain top to one of the ~13 baked-in trusted roots
  is *reported* (`TLS✱`/`TLS+`/`TLS?`) but not itself fatal — full root-store
  enforcement (a fatal gate over ~150 roots) stays deferred to protect live
  browsing. The live boot HTTPS self-test (M555) exercises all of this against
  real example.com every boot.
- **Integer JavaScript** — no FPU, so no float/NaN/Infinity: `0.5 → 0`,
  `1/0 → 0` (guarded, no `#DE`), `undefined + 1 → 1`. Intentional.
- **Regex** — backreferences, lookahead, and `{n,m}` are unsupported; they
  degrade to a graceful no-match (never crash/hang), and the matcher has a step
  budget + depth cap (ReDoS-safe on a 256 KB stack).
- **Browser** — token-stream renderer, no CSS layout engine; remote images are
  clickable links (local images decode inline).

Architectural items beyond these (a CSS/layout engine, `Symbol`/generators/
`Proxy`, robust big-CDN out-of-order TCP, enforcing cert validation, inline
remote-image fetch, app-exit address-space teardown) are deferred by design —
they change core working systems and warrant a focused, supervised pass.
