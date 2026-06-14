# Milestone 124 — big-integer math + RSA signature verification

**Goal:** be able to *verify* a certificate signature, not just parse the cert
(milestone 123). The common CA signing scheme is **RSA PKCS#1 v1.5**, which needs
big-integer modular exponentiation — so this adds a small bignum library and RSA
verification on top of it.

## Big integers (`bignum.c`)

Fixed-size unsigned big integers as little-endian 32-bit limb arrays with a
tracked length; 64-bit products keep the schoolbook multiply integer-only (the
kernel has no FPU). The pieces RSA needs:

- load/store big-endian bytes, compare,
- multiply (schoolbook), and modular reduction by **bit-by-bit long division**
  (simple and obviously correct — speed is irrelevant for an occasional cert
  check),
- **`bn_modexp`** (square-and-multiply): `base^exp mod m`.

**Verified vs Python:** 400 random `pow(base, exp, mod)` cases across 256- to
2048-bit moduli and exponents (including the RSA `e = 65537` and full-width
exponents) — every result matches Python's arbitrary-precision integers. Clean
under ASan + UBSan.

## RSA verification (`rsa.c`)

- **`rsa_pubkey_parse`** pulls `(n, e)` out of an `RSAPublicKey` DER
  (`SEQUENCE { INTEGER n, INTEGER e }`, stripping the ASN.1 sign byte).
- **`rsa_pkcs1_sha256_verify`** computes `s^e mod N` and checks the recovered
  block is a well-formed PKCS#1 v1.5 EMSA: `00 01 FF…FF(≥8) 00`, the SHA-256
  DigestInfo prefix, then the 32-byte hash — each step validated.

- **`rsa_pss_sha256_verify`** does **RSASSA-PSS** (SHA-256, MGF1-SHA-256,
  32-byte salt) per RFC 8017 §9.1.2 — the scheme **TLS 1.3's CertificateVerify**
  uses for RSA keys. It modexp's the signature, then runs EMSA-PSS-VERIFY: check
  the `0xbc` trailer, unmask DB with MGF1(H), validate the `PS‖0x01‖salt`
  structure, and confirm `SHA256(0x00⁸ ‖ mHash ‖ salt) == H`. (MGF1 is the small
  SHA-256-based mask generator.)

**Verified vs OpenSSL:** generated RSA keys (2048- and 3072-bit) and, for both
**PKCS#1 v1.5** (20 keys) and **PSS** (15 keys), signed random messages with
`EVP_DigestSign` and confirmed every signature **verifies**, while a flipped hash
byte or signature byte is **rejected**. Clean under ASan + UBSan; compiles into
the kernel.

## TLS foundation status

The certificate path can now: parse a cert (m123), extract the RSA public key,
and **verify RSA signatures** over it — both PKCS#1 v1.5 (cert-chain signing)
and PSS (TLS 1.3 CertificateVerify). Still ahead for full validation:
**ECDSA-P256** (for EC certs), then the **handshake + record layer**.

## Files
- `kernel/bignum.c`, `kernel/include/bignum.h` — `bn_*` + `bn_modexp`
- `kernel/rsa.c`, `kernel/include/rsa.h` — `rsa_pubkey_parse`,
  `rsa_pkcs1_sha256_verify`, `rsa_pss_sha256_verify` (+ MGF1)
