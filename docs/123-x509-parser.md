# Milestone 123 — minimal X.509 / ASN.1 certificate parser

**Goal:** parse the certificate a TLS server sends — extract its **public key**
(and subject + validity) — toward certificate handling for HTTPS. This is the
"read what the server sends" piece; signature/chain *verification* is a separate
later step.

## What it does

X.509 certificates are ASN.1 DER: nested tag-length-value triples. The parser
has a single bounds-checked `tlv()` reader (handling short- and long-form DER
lengths) and walks the structure:

```
Certificate ::= SEQUENCE { tbsCertificate, sigAlg, sigValue }
tbsCertificate ::= SEQUENCE { [0]version, serial, sigAlg, issuer,
                              validity, subject, subjectPublicKeyInfo, ... }
```

It skips to `subjectPublicKeyInfo` and pulls out:
- the **public-key algorithm** (RSA vs EC, by OID),
- the **public key** bytes (the `subjectPublicKey` BIT STRING) and the full
  SubjectPublicKeyInfo DER,
- the **subject commonName** (walking the Name's RDN/SET/AttributeTypeAndValue),
- the **notAfter** validity time.

Because the input is an untrusted server certificate, every length and every
read is bounds-checked; the extracted pointers alias into the caller's buffer
(no copying).

## Verified — OpenSSL and fuzzing

- **vs OpenSSL:** parsed both an **RSA-2048** and an **EC P-256** certificate
  (generated with `openssl req -x509`). For each, the extracted
  SubjectPublicKeyInfo matches OpenSSL's `-pubkey` DER **byte-for-byte**, the
  algorithm is classified correctly (RSA / EC), the subject CN matches, and the
  notAfter is extracted.
- **Fuzzed:** ~40,000 truncated + randomly-corrupted certificates under
  ASan + UBSan — every malformed input is cleanly rejected, **zero crashes**
  (the classic vulnerability surface for ASN.1 parsers).
- Compiles into the kernel.

## Where this fits

The TLS client now has its full crypto toolkit (m119–122) **and** can read a
server certificate's key. Still ahead: **signature verification** (ECDSA-P256 /
RSA-PKCS1 — another crypto chunk) for actually *trusting* the certificate, and
the **handshake + record layer** that ties everything to the TCP connection.

## Files
- `kernel/x509.c`, `kernel/include/x509.h` — `x509_parse` (+ the `x509_cert`
  struct: SPKI, key, key_alg, subject_cn, not_after)
