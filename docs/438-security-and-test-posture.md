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

## Committed test coverage — `make check` (6 suites, ~5 s, ASan+UBSan)

| Suite | Locks |
|---|---|
| `jstest`  | ~60 JS features vs a golden, incl. ToPrimitive/coercion + div-by-zero-guard semantics |
| `imgtest` | jpeg/png/gif/inflate fuzz + the M422 PoC |
| `x509test`| cert DER parser fuzz |
| `nettest` | packet parser + OOO reassembly fuzz |
| `fstest`  | FAT32 read (corrupt/cyclic images) + write (heavy-churn) — locks M435 + the cycle/depth caps |
| `kattest` | crypto vs RFC/FIPS known-answer vectors (SHA-256/384/512, HMAC, HKDF, AES, AES-GCM, ChaCha20-Poly1305 + forged-tag reject, X25519) |

Each is verified to catch a reintroduced bug (e.g. removing fat32's dir-recursion
depth caps trips an ASan stack-overflow; any crypto-output change fails its
vector; the image PoC aborts if the DRI guard is removed). `make test` is a
*separate* headless boot smoke test, not these.

## Honest known limits (not bugs — documented design choices)

- **Cert validation is informational, not enforcing.** The chain is built,
  issuer links + `CertificateVerify` are checked, and anchoring to a baked-in
  root is *reported* (`TLS✱`/`TLS+`/`TLS?`), but a failure isn't fatal —
  enforcement needs ~150 baked roots + a fatal gate (deferred to protect live
  browsing).
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
