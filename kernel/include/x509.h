/* x509.h — minimal X.509 certificate field extraction (see x509.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { X509_KEY_OTHER = 0, X509_KEY_RSA, X509_KEY_EC };
/* signatureAlgorithm of the cert (how the ISSUER signed this cert) */
enum { X509_SIG_OTHER = 0, X509_SIG_RSA_SHA256, X509_SIG_ECDSA_SHA256,
       X509_SIG_RSA_SHA384, X509_SIG_ECDSA_SHA384 };

#define X509_MAX_SAN  16     /* cap on stored subjectAltName dNSName entries per cert */
#define X509_SAN_LEN  64     /* cap per dNSName incl. NUL (covers all real hostnames) */

typedef struct {
    const uint8_t *spki; size_t spki_len;   /* full SubjectPublicKeyInfo DER */
    int            key_alg;                  /* X509_KEY_RSA / X509_KEY_EC / OTHER */
    const uint8_t *key;  size_t key_len;     /* the public key BIT STRING contents */
    char           subject_cn[64];           /* subject commonName, "" if none */
    char           not_after[24];            /* validity notAfter (UTCTime/GeneralizedTime) */
    const uint8_t *tbs;  size_t tbs_len;     /* tbsCertificate DER (the signed region) */
    const uint8_t *sig;  size_t sig_len;     /* signatureValue contents (BIT STRING, no unused-bits byte) */
    int            sig_alg;                  /* X509_SIG_* (how the issuer signed this cert) */
    char           san[X509_MAX_SAN][X509_SAN_LEN];  /* subjectAltName dNSName entries (each NUL-terminated) */
    int            n_san;                    /* number of valid entries in san[] (0 if none) */
    int            san_capped;               /* 1 if there were more SAN entries than we stored (cap hit) */
} x509_cert;

/* Parse a DER certificate, filling `out`. Returns 0 on success, -1 on malformed
 * input. Pointers in `out` alias into `der` (no copy); bounds-checked. */
int x509_parse(const uint8_t *der, size_t len, x509_cert *out);

/* 1 if `host` matches the certificate's identity (RFC 6125): against the SAN
 * dNSNames if any are present (CN ignored), else the subject CN; case-insensitive,
 * with single-leftmost-label wildcard support. 0 on no match / empty host. */
int host_matches_cert(const char *host, const x509_cert *cert);
