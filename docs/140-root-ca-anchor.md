# Milestone 140 — root-CA trust store + chain anchoring

**Goal:** the last conceptual piece of certificate-path validation. Milestone 139
verified the chain is internally consistent (each cert signed by the next); this
anchors the top of that chain to a **trusted root CA** baked into the kernel — so
for the first time the client checks not just *"is this chain self-consistent"* but
*"does it terminate at a root I trust."*

## How

- **`kernel/rootca.c` / `rootca.h`** — a tiny trust store: an array of
  `{ name, public-key, key_alg }`. The first entry is **ISRG Root X1** (Let's
  Encrypt, RSA-4096), whose key was extracted with `openssl` and baked in as the
  `RSAPublicKey` DER (the same form `x509_cert.key` uses). Adding a root is a
  one-liner + its key bytes.
- **`tls.c`** factored the per-cert signature check into `cert_sig_ok(cert,
  issuer_key, issuer_key_len, issuer_key_alg)` (SHA-256 of the cert's TBS, then
  `rsa_pkcs1_sha256_verify` / `ecdsa_p256_verify_der`). It's used both for the
  chain links *and* for anchoring: after the chain loop, the **top** presented
  cert is checked against each root key — if its signature verifies against a
  trusted root, the chain is anchored.

This still **logs, doesn't enforce** (the result is recorded in `chain_anchored`,
not used to abort) — making it fatal needs broader algorithm coverage (SHA-384 /
P-384) and more roots so legitimate sites aren't false-rejected.

## Verified — live, to a real trusted root

- **text.npr.org**: `www.npr.org` (RSA) ← **R13** (Let's Encrypt) ← **ISRG Root
  X1** — `chain: 1/1 verified` then **`chain ANCHORED to trusted root: ISRG Root
  X1`**, page renders. The full path is validated with from-scratch crypto, and
  the RSA-4096 root-signature verify runs fine on the 256 KB stack (no overflow,
  no hang — the heaviest crypto the OS does).
- **example.com**: the EC chain's leaf link verifies but the SSL.com CA-to-CA
  links use SHA-384/P-384, so it isn't anchored to our (ISRG-only) store — honestly
  reported, not falsely claimed.

## Next (to enforce it)

1. **SHA-384 + ECDSA P-384** so non-Let's-Encrypt chains fully verify.
2. More roots in the store (DigiCert, SSL.com, etc.).
3. Make CertVerify + full chain + trusted-root **fatal** (reject otherwise), with
   a clear in-UI "secure" vs "unverified" distinction.

## Surfaced in the UI

The browser's status indicator now shows the validation *level* honestly, per
HTTPS page: **`TLS*`** = the chain validated all the way to a trusted root CA;
**`TLS+`** = the server proved leaf-key possession but the chain isn't anchored to
a known root; **`TLS?`** = a check failed. (Verified: NPR shows `TLS*`,
example.com shows `TLS+`.) `tls_chain_anchored()` exposes the result.

## Files
- `kernel/rootca.c`, `kernel/include/rootca.h` — the trust store (ISRG Root X1)
- `kernel/tls.c` — `cert_sig_ok` helper; anchors the chain top to the root store;
  `tls_chain_anchored()`
- `kernel/browser.c` — `TLS*`/`TLS+`/`TLS?` status indicator
