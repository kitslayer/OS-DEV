/* x509.h — minimal X.509 certificate field extraction (see x509.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { X509_KEY_OTHER = 0, X509_KEY_RSA, X509_KEY_EC };
/* signatureAlgorithm of the cert (how the ISSUER signed this cert) */
enum { X509_SIG_OTHER = 0, X509_SIG_RSA_SHA256, X509_SIG_ECDSA_SHA256,
       X509_SIG_RSA_SHA384, X509_SIG_ECDSA_SHA384 };

typedef struct {
    const uint8_t *spki; size_t spki_len;   /* full SubjectPublicKeyInfo DER */
    int            key_alg;                  /* X509_KEY_RSA / X509_KEY_EC / OTHER */
    const uint8_t *key;  size_t key_len;     /* the public key BIT STRING contents */
    char           subject_cn[64];           /* subject commonName, "" if none */
    char           not_after[24];            /* validity notAfter (UTCTime/GeneralizedTime) */
    const uint8_t *tbs;  size_t tbs_len;     /* tbsCertificate DER (the signed region) */
    const uint8_t *sig;  size_t sig_len;     /* signatureValue contents (BIT STRING, no unused-bits byte) */
    int            sig_alg;                  /* X509_SIG_* (how the issuer signed this cert) */
} x509_cert;

/* Parse a DER certificate, filling `out`. Returns 0 on success, -1 on malformed
 * input. Pointers in `out` alias into `der` (no copy); bounds-checked. */
int x509_parse(const uint8_t *der, size_t len, x509_cert *out);
