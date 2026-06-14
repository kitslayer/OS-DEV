# Milestone 143 — expanded trust store + key-match anchoring

**Goal:** with the crypto now covering RSA + ECDSA(P-256/P-384) × SHA-256/384
(milestones 138–142), the only thing stopping non-Let's-Encrypt sites from
anchoring was the trust store (one root) and the anchoring method. This expands
both so real sites across multiple CAs fully validate to a trusted root.

## What

- **More roots** (extracted from the system trust bundle into `rootca.c`):
  **DigiCert Global Root G2** (RSA-2048) and **SSL.com TLS ECC Root CA 2022**
  (EC P-384), alongside ISRG Root X1.
- **Key-match anchoring**: a chain that *includes* its root as the top cert (e.g.
  Cloudflare's example.com chain ends at the SSL.com root) couldn't be anchored by
  "verify the top cert's signature under a root key" alone — that root may be
  self- or cross-signed. So anchoring now also succeeds when the **top cert's
  public key equals a baked-in trusted root key** (the chain terminates at a cert
  we trust). The links below it were already verified up to that key, so this is a
  sound anchor. The original sig-verify path (for chains ending at an intermediate,
  like Let's Encrypt's) is kept.

## Verified — full path validation to a trusted root, multiple CAs

- **example.com**: `chain: 3/3` + **`ANCHORED to SSL.com TLS ECC Root CA 2022`**
  (key-match) — the complete P-384 EC chain validated to a trusted root; the
  browser status shows **`TLS*`**.
- **text.npr.org**: still **`ANCHORED to ISRG Root X1`** (sig-verify) — no
  regression from the key-match addition.
- **www.microsoft.com**: `chain: 2/2` + **`ANCHORED to DigiCert Global Root G2`**
  (its "Microsoft TLS RSA Root G2" chain-top is DigiCert-signed) — a 3rd CA, RSA
  with a SHA-384 leaf link. (Microsoft RSA Root CA 2017 is also in the store for
  Microsoft properties that chain to it directly.)

So all four fetchable test sites now validate fully to a trusted root, exercising
both anchor methods (sig-verify + key-match) and RSA + ECDSA(P-256/P-384) +
SHA-256/384. Trust store: ISRG Root X1, DigiCert Global Root G2, SSL.com TLS ECC
Root CA 2022, Microsoft RSA Root CA 2017.

So the from-scratch browser now performs **full certificate-path validation to a
trusted root** for real HTTPS sites across Let's Encrypt, Cloudflare/SSL.com, and
(in the store) DigiCert — with from-scratch RSA + ECDSA(P-256/P-384) + SHA-256/384.

## Next (to enforce)

A production browser ships ~150 roots; we have 3 (representative). Enforcement
(rejecting unvalidated chains) would need a fuller store + gating acceptance on
`CertVerify && all-links-verified && anchored`. Until then it stays informational
(the `TLS*`/`TLS+`/`TLS?` indicator).

## Review (subagent #31)

Reviewed the P-384 work (m142) + this milestone: **no security defects**. The
P-256 refactor is behavior-preserving, the `mul` bit-length fix is correct, and
**key-match anchoring is sound** — an attacker presenting a cert that carries a
trusted root's *public* key as the chain top gains nothing, because the link below
it only verifies if it was signed by that root's *private* key (which they lack),
and the leaf→CertVerify binds the server's key. Added a defensive `hashlen <= 48`
guard in `ecdsa_cert_verify` (per the review; unreachable with current callers).
Forward-looking note for *enforcement*: gate trust on
`anchor && all-links-verified (ok==links) && CertVerify-ok` — the anchor alone
proves only that the top is a trusted root, not that the leaf chains to it.

## Files
- `kernel/rootca.c` — DigiCert Global Root G2 + SSL.com TLS ECC Root CA 2022
- `kernel/tls.c` — anchoring also matches the top cert's key against the store
