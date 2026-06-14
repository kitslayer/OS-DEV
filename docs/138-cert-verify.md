# Milestone 138 — TLS CertificateVerify validation (server key proof)

**Goal:** the documented HTTPS security gap. The TLS 1.3 client (m127) verified
the server's *Finished* (proving it derived the same keys) but never checked the
**CertificateVerify** — the signature, made with the leaf certificate's private
key, over the handshake transcript. Without it the server never proves it owns the
certificate it presented. This adds that check, using the from-scratch X.509 +
ECDSA/RSA code (m123–125), and in doing so validates that crypto against *real*
certificates for the first time.

## The blocker: worker stack overflow

A first attempt **double-faulted (rsp=0 → stack overflow)** the instant it ran the
verify: bignum (`uint32[260]` ≈ 1 KB per number), RSA modexp, and ECDSA point math
have stack frames far larger than the fetch worker's default **16 KB** task stack.
Fixed by adding `task_create_stack(entry, cr3, proc, stack_size)` and giving the
browser's fetch worker a **256 KB** stack — it's the one task that runs heavy
crypto. (`task_create` is now a wrapper passing the old 16 KB default.) *Lesson,
reaffirmed: heavy/deep code needs a sized stack; the default-stack + no-guard-page
combination silently smashes.*

## The check (RFC 8446 §4.4.2–4.4.3)

In the handshake-flight loop:
- **Certificate**: `tls_capture_leaf_key` parses the message (bounds-checking the
  1/3/3-byte length fields), runs `x509_parse` on the leaf cert, and **copies** its
  public key into the `tls` struct (the reassembly buffer moves between records, so
  it can't be aliased).
- **CertificateVerify**: snapshot the transcript hash *through Certificate* (before
  adding CertVerify), rebuild the signed content — `64×0x20 ‖ "TLS 1.3, server
  CertificateVerify" ‖ 0x00 ‖ transcript-hash` — SHA-256 it, and verify the
  signature against the leaf key: `ecdsa_p256_verify_der` for
  `ecdsa_secp256r1_sha256`, `rsa_pss_sha256_verify` / `rsa_pkcs1_sha256_verify` for
  the RSA schemes.

It is **non-fatal / logged, not enforced**: we don't yet validate the cert *chain*
to a trust anchor, so this proves key-possession but not identity (no MITM
protection until chain validation lands). Logging keeps the working browser safe
regardless of the result.

## Verified

Live, no panics: **`[tls] CertificateVerify: signature OK (leaf key proven)`** on
**example.com** (Cloudflare), **text.npr.org** (NPR), and **www.gnu.org** — three
different cert profiles — and each page still renders. Both signature paths are
exercised against real certs: ECDSA-P256 (example.com/NPR) **and** RSA-PSS — the
leaf-cert logging shows `www.gnu.org` actually serves an RSA cert
(`CN=wildebeest1p.gnu.org`, expires 2026-08-07), and `rsa_pss_sha256_verify`
validates it (RSA modexp now fits the 256 KB stack). The from-scratch
ECDSA/RSA/X.509 code validates real-world certificates, not just test vectors.

`tls_capture_leaf_key` also logs the leaf cert's subject CN + notAfter, which both
surfaces real cert metadata and confirms `x509_parse`'s CN/expiry extraction works
on real certificates.

## Review hardening (subagent review #28)

The review confirmed the new parsing glue is bounds-safe (every wire length is
checked before use; `signed_content` can't overflow; the leaf-key copy is bounded;
the transcript snapshot is correctly timed; the check is genuinely non-fatal). It
caught one real regression I'd introduced (**MEDIUM**): the 256 KB stack covered
the *browser worker* but **not the `SYS_https` syscall path** — the shell's
`get`/`wget https` run `tls_get` (now incl. the heavy crypto) on the *app's* kernel
stack, which was still the 16 KB default, so `get https://…` would stack-overflow.
Fixed by giving **ring-3 app tasks** a 256 KB kernel stack too (`app.c`); verified
`get https://example.com` from the shell now runs the cert verify and prints the
response with no panic.

## Surfaced in the UI

`tls_cert_status()` exposes the most recent handshake's result; the browser stores
it per-page and the status line shows a compact, honest indicator on HTTPS pages:
**`TLS+`** (server proved leaf-key possession) or **`TLS?`** (that check failed).
It deliberately does *not* say "secure"/"trusted" — there's no chain validation
yet — and the full detail (cert CN, expiry, verify result) is in the serial log.

## Files
- `kernel/task.c`, `kernel/include/task.h` — `task_create_stack`
- `kernel/browser.c` — fetch worker gets a 256 KB stack
- `kernel/app.c` — ring-3 app tasks get a 256 KB kernel stack (for `SYS_https`)
- `kernel/tls.c`, `kernel/include/tls.h` — `tls_capture_leaf_key` +
  `tls_verify_certverify` (wired into the flight loop); `tls_cert_status()`
- `kernel/browser.c` — stores `cert_status`, shows the `TLS+`/`TLS?` status hint
