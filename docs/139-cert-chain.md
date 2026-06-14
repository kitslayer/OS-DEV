# Milestone 139 — X.509 chain-internal signature verification

**Goal:** continue the certificate-authentication story past milestone 138.
CertificateVerify proves the server holds the *leaf* cert's key; the next step
toward real trust is verifying the **certificate chain** — that each cert was
signed by the one above it, up to a trusted root. This milestone does the
chain-*internal* part (each cert signed by the next), **non-fatal / logged**,
validating the chain crypto against real CA signatures without risking the
working browser. (The trust-anchor / root-CA-store step, and making it fatal,
come next.)

## How

- **`x509.c` now also exposes** each cert's `tbs` (the tbsCertificate DER — the
  signed region), `sig` (signatureValue), and `sig_alg`
  (`sha256WithRSAEncryption` or `ecdsa-with-SHA256`). These are captured during
  the existing bounds-checked TLV walk; if anything is unexpected the cert is
  simply treated as unverifiable.
- **`tls_capture_leaf_key`** now parses the *whole* chain into an array (all cert
  pointers alias the flight buffer, valid for the single call that processes the
  Certificate message), captures the leaf key as before, and then verifies each
  adjacent link: `SHA-256(cert[i].tbs)` checked against `cert[i].sig` using
  `cert[i+1]`'s public key — `rsa_pkcs1_sha256_verify` for RSA-SHA256,
  `ecdsa_p256_verify_der` for ECDSA-P256-SHA256. The heavy verify runs on the
  256 KB worker/app stacks (milestone 138).

## Verified — live, against real chains

- **www.gnu.org**: `wildebeest1p.gnu.org` (RSA) ← **R12** (Let's Encrypt) —
  `chain: 1/1 issuer link(s) verified` (RSA-SHA256).
- **text.npr.org**: `www.npr.org` (RSA) ← **R13** (Let's Encrypt) — `1/1` verified.
- **example.com**: a 4-cert EC chain (`example.com` ← Cloudflare ECC CA 3 ←
  SSL.com Transit ← SSL.com Root) — `1/3`: the leaf link verifies via
  ECDSA-P256-SHA256; the two CA-to-CA links are honestly reported "unsupported sig
  alg" because they use SHA-384 / P-384, which the kernel doesn't have yet.

No panics. The from-scratch RSA and ECDSA verify code validates **real CA
signatures on real certificates**, not just synthetic vectors.

## Next (to make this enforceable for MITM protection)

1. **SHA-384** (many CA-to-CA links sign with it) and **ECDSA P-384** (CA keys) —
   so full chains verify, not just the leaf link.
2. A baked-in **root-CA trust store** + match the chain's top to it.
3. Then make the whole check (CertVerify + chain + root) **fatal**.

## Files
- `kernel/x509.c`, `kernel/include/x509.h` — expose `tbs` / `sig` / `sig_alg`
- `kernel/tls.c` — parse the full chain + verify each issuer link (logged)
