/* rootca.h — a tiny baked-in trust store of root-CA public keys, used to anchor
 * a verified TLS certificate chain to a trusted root (see tls.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char    *name;       /* human-readable, for logging */
    const uint8_t *key;        /* the public key in the same form x509_cert.key uses
                                * (RSA: RSAPublicKey DER; EC: 0x04‖X‖Y point) */
    size_t         key_len;
    int            key_alg;    /* X509_KEY_RSA / X509_KEY_EC */
} root_ca;

extern const root_ca ROOT_CAS[];
extern const int      N_ROOT_CAS;
