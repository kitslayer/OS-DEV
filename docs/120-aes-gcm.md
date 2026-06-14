# Milestone 120 — AES-128-GCM (the TLS AEAD)

**Goal:** the second crypto piece toward HTTPS. After the key exchange (X25519,
milestone 119), TLS protects each record with an **AEAD** cipher. The most
widely-supported one is **AES-128-GCM**, and we already have AES-128 — so this
adds GCM on top.

## What it is

GCM = counter-mode encryption + a Galois-field MAC:

- **GCTR** encrypts the plaintext with AES in counter mode, the counter starting
  at `J0 + 1` where `J0 = IV‖0x00000001` (TLS uses a 96-bit IV).
- **GHASH** authenticates the AAD and ciphertext: it folds each 16-byte block
  into an accumulator with a carry-less multiply by `H = AES_K(0)` in
  GF(2¹²⁸). The multiply is done **bit by bit** (128 shift/conditional-XOR/
  reduce steps) — no `PCLMULQDQ` needed, so it's integer-only and kernel-safe.
- The **tag** is `GHASH(AAD, C, lengths) ⊕ AES_K(J0)`. Decryption recomputes the
  tag over the ciphertext and compares it in **constant time** before releasing
  the plaintext.

It's built on the existing `aes128_encrypt_block`, so the block cipher isn't
duplicated.

## Verified — against OpenSSL

A host test compares `aes128_gcm_encrypt` to **OpenSSL's** `EVP_aes_128_gcm`
over **4000 random cases** (every plaintext length 0–199, AAD length 0–47,
random keys and IVs): the ciphertext **and** tag match in every case. A second
run did 3000 encrypt→decrypt round-trips (all recovered the plaintext) and 3000
bit-flip tamper tests (**all 3000 rejected**). Clean under ASan + UBSan +
signed-integer-overflow; compiles into the kernel with `-mgeneral-regs-only`.

## Crypto inventory toward TLS

With this, the building blocks are: **X25519** (ECDH key exchange), **AES-128**
+ **AES-128-GCM** (record encryption/AEAD), and **SHA-256**. Still ahead for a
minimal HTTPS client: **HMAC-SHA256** + the TLS key schedule (PRF / HKDF),
**X.509 / ASN.1** certificate parsing, and the **handshake + record layer**.

## Files
- `kernel/aesgcm.c`, `kernel/include/aesgcm.h` — `aes128_gcm_encrypt` /
  `aes128_gcm_decrypt` (GCTR + bit-by-bit GHASH)
