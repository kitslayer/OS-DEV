/*
 * x509.c — a minimal X.509 / ASN.1 DER parser: enough to pull a server
 * certificate's public key (and subject CN + notAfter) out, toward TLS
 * certificate handling. Parses UNTRUSTED input (a server's cert chain), so
 * every length and read is bounds-checked. It does not yet verify signatures
 * or chains — it extracts fields. Host-tested against OpenSSL and fuzzed.
 */
#include "x509.h"
#include "string.h"

/* DER tags */
#define T_INT   0x02
#define T_BITS  0x03
#define T_OID   0x06
#define T_UTF8  0x0c
#define T_SEQ   0x30
#define T_SET   0x31
#define T_CTX0  0xA0       /* [0] EXPLICIT */

/* Read one TLV at *p (bounded by end): set tag + content pointer/length and
 * advance *p past the whole element. Returns 0, or -1 on any malformation. */
static int tlv(const uint8_t **p, const uint8_t *end, int *tag,
               const uint8_t **cval, size_t *clen) {
    if (*p + 2 > end) return -1;
    int t = *(*p)++;
    size_t len; int b = *(*p)++;
    if (b < 0x80) len = (size_t)b;
    else {
        int nb = b & 0x7f;
        if (nb == 0 || nb > 4 || *p + nb > end) return -1;
        len = 0;
        for (int i = 0; i < nb; i++) len = (len << 8) | *(*p)++;
    }
    if (len > (size_t)(end - *p)) return -1;
    *tag = t; *cval = *p; *clen = len; *p += len;
    return 0;
}

/* rsaEncryption (1.2.840.113549.1.1.1) and id-ecPublicKey (1.2.840.10045.2.1) */
static const uint8_t OID_RSA[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01};
static const uint8_t OID_EC[]  = {0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01};
static const uint8_t OID_CN[]  = {0x55,0x04,0x03};            /* commonName 2.5.4.3 */
/* signature algorithms we can verify: sha256WithRSAEncryption (1.2.840.113549.1.1.11)
 * and ecdsa-with-SHA256 (1.2.840.10045.4.3.2). */
static const uint8_t OID_RSA_SHA256[]   = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B};
static const uint8_t OID_ECDSA_SHA256[] = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02};
/* sha384WithRSAEncryption (1.2.840.113549.1.1.12) and ecdsa-with-SHA384 (1.2.840.10045.4.3.3) */
static const uint8_t OID_RSA_SHA384[]   = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C};
static const uint8_t OID_ECDSA_SHA384[] = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03};

static int oid_eq(const uint8_t *o, size_t ol, const uint8_t *ref, size_t rl) {
    return ol == rl && memcmp(o, ref, rl) == 0;
}

/* Search a Name (SEQUENCE OF RDN SET OF AttrTypeAndValue) for the commonName. */
static void find_cn(const uint8_t *p, const uint8_t *end, char *cn, int cnsz) {
    cn[0] = 0;
    while (p < end) {
        int tag; const uint8_t *c; size_t cl;
        if (tlv(&p, end, &tag, &c, &cl) != 0) return;        /* each RDN: a SET */
        if (tag != T_SET) continue;
        const uint8_t *sp = c, *send = c + cl;
        while (sp < send) {
            int t2; const uint8_t *c2; size_t l2;
            if (tlv(&sp, send, &t2, &c2, &l2) != 0) break;   /* AttributeTypeAndValue SEQ */
            if (t2 != T_SEQ) continue;
            const uint8_t *ap = c2, *aend = c2 + l2;
            int t3; const uint8_t *oid; size_t oidl;
            if (tlv(&ap, aend, &t3, &oid, &oidl) != 0 || t3 != T_OID) continue;
            int t4; const uint8_t *val; size_t vl;
            if (tlv(&ap, aend, &t4, &val, &vl) != 0) continue;
            if (oid_eq(oid, oidl, OID_CN, sizeof OID_CN)) {
                int n = (int)vl; if (n > cnsz - 1) n = cnsz - 1;
                for (int i = 0; i < n; i++) cn[i] = (char)val[i];
                cn[n] = 0;
                return;
            }
        }
    }
}

int x509_parse(const uint8_t *der, size_t len, x509_cert *out) {
    memset(out, 0, sizeof(*out));
    const uint8_t *p = der, *end = der + len;
    int tag; const uint8_t *c; size_t cl;

    if (tlv(&p, end, &tag, &c, &cl) != 0 || tag != T_SEQ) return -1;   /* Certificate */
    const uint8_t *tp = c, *tend = c + cl;
    const uint8_t *tbs0 = tp;                                          /* signed region start */
    if (tlv(&tp, tend, &tag, &c, &cl) != 0 || tag != T_SEQ) return -1; /* tbsCertificate */
    out->tbs = tbs0; out->tbs_len = (size_t)(tp - tbs0);               /* full tbsCertificate element */

    const uint8_t *q = c, *qend = c + cl;
    const uint8_t *f; size_t fl;
    if (tlv(&q, qend, &tag, &f, &fl) != 0) return -1;
    if (tag == T_CTX0) { if (tlv(&q, qend, &tag, &f, &fl) != 0) return -1; }  /* skip [0] version */
    /* `tag/f` now hold serialNumber (INTEGER) — skip it and the next two SEQs */
    if (tlv(&q, qend, &tag, &f, &fl) != 0) return -1;                  /* signature alg */
    if (tlv(&q, qend, &tag, &f, &fl) != 0) return -1;                  /* issuer Name */

    if (tlv(&q, qend, &tag, &f, &fl) != 0 || tag != T_SEQ) return -1;  /* validity */
    {   const uint8_t *vp = f, *vend = f + fl; int vt; const uint8_t *vc; size_t vcl;
        if (tlv(&vp, vend, &vt, &vc, &vcl) == 0)                       /* notBefore */
            if (tlv(&vp, vend, &vt, &vc, &vcl) == 0) {                 /* notAfter */
                int n = (int)vcl; if (n > (int)sizeof(out->not_after) - 1) n = sizeof(out->not_after) - 1;
                for (int i = 0; i < n; i++) out->not_after[i] = (char)vc[i];
                out->not_after[n] = 0;
            }
    }

    if (tlv(&q, qend, &tag, &f, &fl) != 0 || tag != T_SEQ) return -1;  /* subject Name */
    find_cn(f, f + fl, out->subject_cn, (int)sizeof(out->subject_cn));

    const uint8_t *spki_start = q;
    if (tlv(&q, qend, &tag, &f, &fl) != 0 || tag != T_SEQ) return -1;  /* SubjectPublicKeyInfo */
    out->spki = spki_start; out->spki_len = (size_t)(q - spki_start);

    /* inside SPKI: AlgorithmIdentifier SEQ (has the OID), then the key BIT STRING */
    const uint8_t *sp = f, *send = f + fl;
    if (tlv(&sp, send, &tag, &c, &cl) != 0 || tag != T_SEQ) return -1; /* algorithm */
    {   const uint8_t *ap = c, *aend = c + cl; int at; const uint8_t *oid; size_t oidl;
        if (tlv(&ap, aend, &at, &oid, &oidl) != 0 || at != T_OID) return -1;
        if (oid_eq(oid, oidl, OID_RSA, sizeof OID_RSA)) out->key_alg = X509_KEY_RSA;
        else if (oid_eq(oid, oidl, OID_EC, sizeof OID_EC)) out->key_alg = X509_KEY_EC;
        else out->key_alg = X509_KEY_OTHER;
    }
    if (tlv(&sp, send, &tag, &c, &cl) != 0 || tag != T_BITS) return -1;/* subjectPublicKey */
    if (cl < 1) return -1;
    out->key = c + 1;                                                   /* skip the unused-bits byte */
    out->key_len = cl - 1;

    /* signatureAlgorithm + signatureValue follow tbsCertificate in the Certificate
     * SEQUENCE (tp now points past tbsCertificate). Best-effort: leave sig_alg = OTHER
     * if anything is off — chain verification just treats that cert as unverifiable. */
    int st; const uint8_t *sc; size_t scl;
    if (tlv(&tp, tend, &st, &sc, &scl) == 0 && st == T_SEQ) {           /* signatureAlgorithm */
        const uint8_t *ap = sc, *aend = sc + scl; int at; const uint8_t *oid; size_t oidl;
        if (tlv(&ap, aend, &at, &oid, &oidl) == 0 && at == T_OID) {
            if (oid_eq(oid, oidl, OID_RSA_SHA256, sizeof OID_RSA_SHA256))        out->sig_alg = X509_SIG_RSA_SHA256;
            else if (oid_eq(oid, oidl, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256)) out->sig_alg = X509_SIG_ECDSA_SHA256;
            else if (oid_eq(oid, oidl, OID_RSA_SHA384, sizeof OID_RSA_SHA384))     out->sig_alg = X509_SIG_RSA_SHA384;
            else if (oid_eq(oid, oidl, OID_ECDSA_SHA384, sizeof OID_ECDSA_SHA384)) out->sig_alg = X509_SIG_ECDSA_SHA384;
        }
    }
    if (tlv(&tp, tend, &st, &sc, &scl) == 0 && st == T_BITS && scl >= 1) {  /* signatureValue */
        out->sig = sc + 1; out->sig_len = scl - 1;                      /* skip unused-bits byte */
    }
    return 0;
}
