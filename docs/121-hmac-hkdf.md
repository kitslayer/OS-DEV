# Milestone 121 — HMAC-SHA256 and HKDF (the TLS key schedule's core)

**Goal:** the third crypto piece toward HTTPS. TLS turns the shared secret from
the key exchange into the actual record keys with a **key-derivation function**;
TLS 1.3's is built entirely on **HKDF** (RFC 5869), which in turn is built on
**HMAC** (RFC 2104). Both are added here, on top of the existing SHA-256.

## What it is

- **`hmac_sha256(key, msg)`** — the standard keyed hash: `H((K⊕opad) ‖
  H((K⊕ipad) ‖ msg))`. Because our SHA-256 is one-shot (not streaming), HMAC
  assembles the padded-key‖message into one buffer and hashes it; the message is
  bounded, which is fine — every HMAC in the TLS 1.3 key schedule is over a small
  input (a transcript hash, a label, a counter).
- **`hkdf_extract(salt, ikm)`** → a 32-byte pseudo-random key (`HMAC(salt, IKM)`).
- **`hkdf_expand(prk, info, L)`** → `L` bytes of output key material, the
  iterated `T(i) = HMAC(PRK, T(i-1) ‖ info ‖ i)` construction.
- **`hkdf_expand_label(secret, label, context, L)`** — TLS 1.3's
  `HKDF-Expand-Label` (RFC 8446 §7.1): HKDF-Expand with a structured info of
  `uint16(L) ‖ opaque("tls13 "+label) ‖ opaque(context)`. **`tls13_derive_secret`**
  is the `Derive-Secret` wrapper (Expand-Label over a transcript hash). These
  are what the whole TLS 1.3 key schedule is expressed in.

## Verified — RFC 5869 and OpenSSL

- **RFC 5869 Test Case 1:** the PRK and the 42-byte OKM match the published
  vector exactly.
- **HMAC vs OpenSSL:** 3000 random (key, message) pairs — every digest matches
  `HMAC(EVP_sha256, …)`.
- **HKDF vs OpenSSL:** 1000 random extract+expand cases (random salt/IKM/info,
  output lengths 1–80) — every result matches OpenSSL's `EVP_PKEY_HKDF`.
- **HKDF-Expand-Label vs OpenSSL:** 2000 random cases (real TLS labels like
  `c hs traffic`/`key`/`iv`, varied contexts and lengths) — every result matches
  OpenSSL's `TLS13-KDF` (expand-only mode).
- Clean under ASan + UBSan; compiles into the kernel.

## Crypto inventory toward TLS

Now in hand: **X25519** (key exchange), **AES-128 + AES-128-GCM** (record
AEAD), **SHA-256**, and **HMAC-SHA256 + HKDF** (key schedule). What remains for
a minimal TLS 1.3 HTTPS client: **X.509 / ASN.1** certificate parsing (and at
least extracting the server's identity), and the **handshake + record layer**
(ClientHello/ServerHello, the HKDF-Expand-Label key schedule, transcript
hashing, and wrapping it over the existing TCP).

## Files
- `kernel/hkdf.c`, `kernel/include/hkdf.h` — `hmac_sha256`, `hkdf_extract`,
  `hkdf_expand`, `hkdf_expand_label`, `tls13_derive_secret`
