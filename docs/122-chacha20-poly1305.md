# Milestone 122 — ChaCha20-Poly1305 (the other TLS AEAD)

**Goal:** complete the AEAD layer. TLS 1.3 defines two mandatory-to-implement
record ciphers — AES-128-GCM (milestone 120) and **ChaCha20-Poly1305**. Some
servers prefer ChaCha20 (it's fast without AES hardware), so supporting both
maximizes which servers we can eventually talk to.

## What it is (RFC 8439)

- **ChaCha20** — an ARX stream cipher: a 16-word state (constants, 256-bit key,
  32-bit block counter, 96-bit nonce) run through 20 rounds of quarter-rounds.
  Pure 32-bit add/rotate/xor — trivially integer-only.
- **Poly1305** — a one-time MAC over the prime field 2¹³⁰−5. Implemented with
  the well-known 5×26-bit-limb "donna" representation and 64-bit products
  (no 128-bit type needed).
- **AEAD** — the Poly1305 key is ChaCha20 keystream block 0; the plaintext is
  encrypted from block 1; the tag authenticates `AAD ‖ pad ‖ ciphertext ‖ pad ‖
  len(AAD) ‖ len(ct)`. Decryption verifies the tag in constant time before
  releasing the plaintext.

## Verified — RFC 8439 and OpenSSL

- **RFC 8439 §2.8.2 worked example:** ciphertext and tag match exactly.
- **vs OpenSSL:** 3000 random cases (plaintext 0–299 bytes, AAD 0–59) — the
  ciphertext **and** tag match `EVP_chacha20_poly1305` every time, and all 3000
  encrypt→decrypt round-trips recover the plaintext.
- **Tamper:** 3000 bit-flipped tags — all 3000 rejected.
- Clean under ASan + UBSan + signed-integer-overflow; compiles into the kernel.

## The TLS 1.3 crypto toolkit is complete

With this, every cryptographic primitive a TLS 1.3 client needs is in place and
independently verified:

| role | primitive | milestone |
|------|-----------|-----------|
| key exchange | X25519 | 119 |
| record AEAD | AES-128-GCM | 120 |
| record AEAD | ChaCha20-Poly1305 | 122 |
| hash / transcript | SHA-256 | (earlier) |
| key schedule | HMAC-SHA256, HKDF, HKDF-Expand-Label | 121 |

What remains is the **protocol** itself: the handshake state machine and record
layer (ClientHello/ServerHello, the key-schedule wiring, transcript hashing) and
X.509 parsing for certificate validation — the integration work, on top of this
verified toolkit.

## Review (whole crypto toolkit)

A read-only security review covered all four primitive files (X25519, AES-GCM,
HKDF, ChaCha20-Poly1305), focusing on what the vector tests can't catch. It
**confirmed**: no out-of-bounds writes anywhere; both AEAD tag comparisons are
**constant-time** (accumulate all 16 byte-diffs, single branch) and release **no
plaintext on authentication failure**; X25519's scalar clamping and the
branch-free `sel25519` conditional swap are correct, and its field limbs stay
well within `int64`. Findings, all fixed or noted:

- **HIGH (fixed):** `hkdf_expand_label` could build an info block up to 514
  bytes, but `hkdf_expand` rejected anything over 256 — so a long label/context
  would have failed derivation (dormant for the short TLS labels actually used).
  `hkdf_expand` now accepts the full 514 bytes its sibling can produce.
- **LOW (hardened):** `hmac_sha256` had a 512-byte message bound (silent
  truncation beyond); raised to 1024 with an explicit documented contract (every
  TLS key-schedule HMAC is far smaller).
- **MEDIUM (noted for the handshake):** X25519 doesn't reject an all-zero
  (low-order-point) shared secret; RFC 7748 permits omitting the check, but the
  TLS key-exchange code should reject it before keying.

## Files
- `kernel/chachapoly.c`, `kernel/include/chachapoly.h` —
  `chacha20poly1305_encrypt` / `_decrypt`
