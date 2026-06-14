/* tls.h — minimal TLS 1.3 HTTPS client (see tls.c). */
#pragma once
#include <stdint.h>

/* HTTPS GET https://host/path over TLS 1.3 -> raw HTTP response into `out`
 * (up to `max` bytes). `seed` seeds the ephemeral-key RNG. Returns bytes
 * received, or -1 on error. */
int tls_get(const char *host, const char *path, uint8_t *out, int max, uint32_t seed);

/* CertificateVerify result of the most recent tls_get: -2 = none/absent,
 * 0 = signature verified (server proved leaf-key possession), -1 = failed.
 * (The full presented chain is parsed + logged; chain-to-root validation is TODO.) */
int tls_cert_status(void);

/* 1 if the most recent tls_get's certificate chain anchored to a trusted root CA
 * (full path validated); 0 otherwise. */
int tls_chain_anchored(void);
