# Milestone 127 — a TLS 1.3 client: the browser fetches the real HTTPS web

**Goal:** the north star. Milestones 119–126 built every piece — X25519 ECDH,
AES-128-GCM and ChaCha20-Poly1305 AEADs, HMAC/HKDF/HKDF-Expand-Label, SHA-256,
the X.509/RSA/ECDSA cert toolkit, and a reusable TCP stream. This milestone wires
them into a working **TLS 1.3 client** (`kernel/tls.c`) and points the browser at
it, so `browse https://example.com` fetches and renders a real page over real
HTTPS from the live internet.

## The handshake (RFC 8446)

`tls_get(host, path, out, max, seed)` runs a full TLS 1.3 handshake over a
`tcp_conn` to port 443:

1. **ClientHello** — legacy TLS 1.2 record version, a fresh X25519 key share,
   `supported_versions=0x0304`, `supported_groups=x25519`, `signature_algorithms`
   (ecdsa_secp256r1 / rsa_pss_rsae / rsa_pkcs1), and the SNI server name.
2. **ServerHello** — parse the negotiated cipher suite (TLS_AES_128_GCM_SHA256 or
   TLS_CHACHA20_POLY1305_SHA256) and the server's X25519 key share.
3. **Key schedule** — `shared = X25519(priv, server_pub)`; then the HKDF ladder:
   `early = Extract(0,0)` → `derived = Derive-Secret(early,"derived",H(""))` →
   `handshake = Extract(derived, shared)`, and from it the client/server
   handshake-traffic secrets (`"c hs traffic"` / `"s hs traffic"`) → per-direction
   `key`/`iv`. A low-order (all-zero) shared secret is rejected.
4. **Decrypt the server flight** — the record layer's AEAD open (nonce = iv XOR
   seq, AAD = the 5-byte record header, inner content-type byte stripped) decrypts
   EncryptedExtensions + Certificate + CertificateVerify + Finished. Records may
   pack several handshake messages back-to-back; we parse each in turn.
5. **Verify server Finished** — recompute `HMAC(finished_key, transcript-hash)`
   over the transcript up to (not including) Finished and compare. This proves the
   server holds the handshake keys derived from the ECDH — i.e. it really
   completed the same handshake we did.
6. **Client Finished** + a middlebox-compat ChangeCipherSpec, then switch to the
   application-traffic keys (`"c ap traffic"` / `"s ap traffic"`).
7. **Application data** — send the HTTP/1.0 GET, read and decrypt the response
   records, and send a `close_notify` alert for a clean shutdown.

## What it verifies — and what it doesn't

The server **Finished** check authenticates that the peer derived the same keys,
so the connection is integrity-protected and confidential against a passive
eavesdropper. The X.509 chain itself is **not** validated yet (no trust-anchor
check / certificate signature verification in the handshake), so this is *not*
proof against an active man-in-the-middle. The building blocks for that
(`x509_parse`, `rsa_*_verify`, `ecdsa_p256_verify`) already exist from milestones
123–125; binding them into the handshake is the remaining hardening step.

## A bug worth recording: the worker stack overflow

The first live attempt crashed with a **General Protection Fault** right after
sending the ClientHello. The cause was not the crypto: `tls_get` declared the
`tls` context (`trans[16384]` + `rbuf[20000]`) plus locals (`inner[20000]`,
`sh[4096]`) **on the stack** — ~80 KB — while the fetch worker task's stack is
only 16 KB (`STACK_SIZE`). The frame ran off the end of the stack into adjacent
heap, and the first deep call (building a TX frame in `tcp_send_seg`) faulted.

Fix: the fetch worker is single-threaded (one `tls_get` at a time, guarded by the
browser's `g_busy`), so the big buffers move to `static` (BSS) instead of the
stack, and a dead unused `abuf[20000]` field was removed. The stack frame drops
to a few KB. *Lesson: a freestanding kernel has no stack guard page — a large
automatic array is a silent stack smash, and the symptom (a GPF in an unrelated
function) points nowhere near the cause.*

## Review hardening (subagent review #24)

A focused security review of `tls.c` against adversarial input found, and we
fixed, before considering it done:

- **CRITICAL — out-of-bounds reads in the ServerHello parser.** The session-id /
  extension lengths are attacker-controlled and were used as indices into the
  4 KB `sh` buffer with no bounds check — a crafted ServerHello could read ~64 KB
  past the buffer. Fixed: every step (`session_id`, cipher suite, extensions
  length, each extension's `el`) is now validated against the actual record
  length `shlen` before any dereference.
- **HIGH — two more large stack arrays.** The original stack-overflow fix missed
  `write_enc`'s `pt[20000]` + `out[20000]` (~40 KB) and `read_enc`'s `rec[20000]`
  — both still blew the 16 KB stack. `write_enc`'s only ever hold ≤512 B (shrunk
  to 2 KB on the stack); `read_enc`'s holds a full record (moved to `static`).
- **HIGH — handshake-message reassembly across records.** `read_enc` returns one
  record at a time; a Certificate split across records (common for real chains)
  had its tail dropped and the transcript corrupted. Fixed: decrypted handshake
  bytes accumulate into a reassembly buffer and only **complete** messages are
  consumed, with the remainder carried forward. The transcript buffer was sized
  up to `40000` for real chains.
- **LOW — Finished length** is now required to be exactly 32 bytes before the
  `memcmp`.

## Verified

- **Host test:** built `tls.c` + the crypto modules against a socket shim and ran
  it against a Python `ssl` TLS 1.3 server — full handshake, server-Finished
  verified, `Hello, TLS!` body received (client exit 0).
- **Live, example.com:** `browse https://example.com` resolved DNS (→ Cloudflare),
  connected on :443, completed the handshake (suite 0x1301, a 3813-byte encrypted
  handshake flight in one record, **server Finished OK**), received 797 bytes, and
  rendered the real "Example Domain" page — heading, body, "Learn more" link.
- **Live, gnu.org (exercises reassembly):** `browse https://www.gnu.org` fetched a
  **15,228-byte** page behind a real Let's Encrypt cert chain and rendered the GNU
  homepage — title, the full navigation tree (ABOUT GNU / PHILOSOPHY / LICENSES /
  …), all links — confirming the multi-record handshake-flight reassembly works
  against a real server with a larger certificate chain. No panics.

## Next

Bind certificate-chain validation into the handshake (parse the Certificate
message, verify CertificateVerify with the leaf key, walk the chain to a trust
anchor) to defeat active MITM; add TLS session-ticket handling and HTTP/1.1
keep-alive; broaden the home page's links.

## Files
- `kernel/tls.c`, `kernel/include/tls.h` — the TLS 1.3 client (`tls_get`)
- `kernel/browser.c` — `worker_fetch` routes `https://` URLs through `tls_get`
- `kernel/task.c` — `STACK_SIZE` (the 16 KB worker stack the fix had to fit)
