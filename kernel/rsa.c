/*
 * rsa.c — RSA PKCS#1 v1.5 signature verification with SHA-256, built on the
 * bignum modexp. Used to verify X.509 certificate signatures (the common CA
 * signing scheme) toward TLS certificate validation. Verified on the host
 * against OpenSSL-produced signatures.
 */
#include "rsa.h"
#include "bignum.h"
#include "sha256.h"
#include "string.h"

/* Parse an RSAPublicKey DER: SEQUENCE { modulus INTEGER, publicExponent INTEGER }.
 * Sets pointers (aliasing into `der`) to the unsigned big-endian n and e. */
int rsa_pubkey_parse(const uint8_t *der, size_t len,
                     const uint8_t **n, size_t *nl, const uint8_t **e, size_t *el) {
    const uint8_t *p = der, *end = der + len;
    if (p + 2 > end || *p != 0x30) return -1;          /* SEQUENCE */
    p++;
    size_t slen; int b = *p++;
    if (b < 0x80) slen = b; else { int nb = b & 0x7f; if (nb<1||nb>4||p+nb>end) return -1; slen=0; for(int i=0;i<nb;i++) slen=(slen<<8)|*p++; }
    if (slen > (size_t)(end - p)) return -1;
    end = p + slen;
    for (int which = 0; which < 2; which++) {          /* two INTEGERs */
        if (p + 2 > end || *p != 0x02) return -1;
        p++;
        size_t ilen; int ib = *p++;
        if (ib < 0x80) ilen = ib; else { int nb = ib & 0x7f; if (nb<1||nb>4||p+nb>end) return -1; ilen=0; for(int i=0;i<nb;i++) ilen=(ilen<<8)|*p++; }
        if (ilen > (size_t)(end - p)) return -1;
        const uint8_t *v = p; size_t vl = ilen;
        while (vl > 1 && v[0] == 0) { v++; vl--; }      /* strip the sign byte */
        if (which == 0) { *n = v; *nl = vl; } else { *e = v; *el = vl; }
        p += ilen;
    }
    return 0;
}

/* DigestInfo prefixes (the ASN.1 preceding the hash in EMSA-PKCS1-v1_5). */
static const uint8_t SHA256_DI[] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};
static const uint8_t SHA384_DI[] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30
};

/* Verify an RSA PKCS#1 v1.5 signature given the hash's DigestInfo prefix +
 * length. 0 = valid, -1 = not. */
static int pkcs1_verify(const uint8_t *n, size_t nl, const uint8_t *e, size_t el,
                        const uint8_t *sig, size_t sl, const uint8_t *di, size_t dilen,
                        const uint8_t *hash, size_t hashlen) {
    if (sl != nl || nl < 11 + dilen + hashlen) return -1;
    bignum N, E, S, M;
    if (bn_from_bytes(&N, n, nl) || bn_from_bytes(&E, e, el) || bn_from_bytes(&S, sig, sl)) return -1;
    if (bn_cmp(&S, &N) >= 0) return -1;                 /* sig must be < modulus */
    bn_modexp(&M, &S, &E, &N);
    uint8_t em[BN_LIMBS * 4];
    if (nl > sizeof(em)) return -1;
    bn_to_bytes(&M, em, nl);                            /* EM = recovered, nl bytes */

    /* EM = 00 01 FF..FF(>=8) 00 DigestInfo Hash */
    if (em[0] != 0x00 || em[1] != 0x01) return -1;
    size_t i = 2;
    while (i < nl && em[i] == 0xFF) i++;
    if (i < 10 || i >= nl || em[i] != 0x00) return -1;  /* >=8 padding bytes, then 00 */
    i++;
    if (nl - i != dilen + hashlen) return -1;
    if (memcmp(em + i, di, dilen) != 0) return -1;
    if (memcmp(em + i + dilen, hash, hashlen) != 0) return -1;
    return 0;
}

/* Verify an RSA PKCS#1 v1.5 signature over a SHA-256 hash. 0 = valid, -1 = not. */
int rsa_pkcs1_sha256_verify(const uint8_t *n, size_t nl, const uint8_t *e, size_t el,
                            const uint8_t *sig, size_t sl, const uint8_t hash[32]) {
    return pkcs1_verify(n, nl, e, el, sig, sl, SHA256_DI, sizeof SHA256_DI, hash, 32);
}

/* Verify an RSA PKCS#1 v1.5 signature over a SHA-384 hash. 0 = valid, -1 = not. */
int rsa_pkcs1_sha384_verify(const uint8_t *n, size_t nl, const uint8_t *e, size_t el,
                            const uint8_t *sig, size_t sl, const uint8_t hash[48]) {
    return pkcs1_verify(n, nl, e, el, sig, sl, SHA384_DI, sizeof SHA384_DI, hash, 48);
}

/* MGF1 with SHA-256: mask = T(0)‖T(1)‖… where T(c) = SHA256(seed ‖ be32(c)). */
static void mgf1(const uint8_t *seed, size_t slen, uint8_t *mask, size_t mlen) {
    uint8_t buf[64], dig[32];
    size_t done = 0; uint32_t c = 0;
    while (done < mlen) {
        size_t sl = slen < sizeof(buf) - 4 ? slen : sizeof(buf) - 4;
        for (size_t i = 0; i < sl; i++) buf[i] = seed[i];
        buf[sl]=c>>24; buf[sl+1]=c>>16; buf[sl+2]=c>>8; buf[sl+3]=(uint8_t)c;
        sha256(buf, sl + 4, dig);
        size_t take = mlen - done; if (take > 32) take = 32;
        for (size_t i = 0; i < take; i++) mask[done + i] = dig[i];
        done += take; c++;
    }
}

/* Verify an RSASSA-PSS signature (SHA-256, MGF1-SHA-256, salt length = 32).
 * Follows EMSA-PSS-VERIFY (RFC 8017 §9.1.2). 0 = valid, -1 = not. */
int rsa_pss_sha256_verify(const uint8_t *n, size_t nl, const uint8_t *e, size_t el,
                          const uint8_t *sig, size_t sl, const uint8_t hash[32]) {
    const size_t hLen = 32, sLen = 32;
    if (sl != nl || nl < hLen + sLen + 2) return -1;
    bignum N, E, S, M;
    if (bn_from_bytes(&N, n, nl) || bn_from_bytes(&E, e, el) || bn_from_bytes(&S, sig, sl)) return -1;
    if (bn_cmp(&S, &N) >= 0) return -1;
    bn_modexp(&M, &S, &E, &N);
    uint8_t em[BN_LIMBS * 4];
    if (nl > sizeof(em)) return -1;
    bn_to_bytes(&M, em, nl);                            /* emLen = nl */

    if (em[nl - 1] != 0xbc) return -1;                  /* trailer */
    size_t dbLen = nl - hLen - 1;
    const uint8_t *maskedDB = em, *H = em + dbLen;
    uint8_t db[BN_LIMBS * 4], dbmask[BN_LIMBS * 4];
    mgf1(H, hLen, dbmask, dbLen);
    for (size_t i = 0; i < dbLen; i++) db[i] = maskedDB[i] ^ dbmask[i];
    /* emBits = 8*nl - 1, so clear the top bit of the first DB byte */
    db[0] &= 0x7f;
    /* DB = PS(0x00…) ‖ 0x01 ‖ salt, with |salt| = sLen */
    size_t ps = dbLen - sLen - 1;
    for (size_t i = 0; i < ps; i++) if (db[i] != 0) return -1;
    if (db[ps] != 0x01) return -1;
    const uint8_t *salt = db + ps + 1;
    /* H' = SHA256( 0x00*8 ‖ mHash ‖ salt );  must equal H */
    uint8_t mp[8 + 32 + 64], hp[32];
    for (int i = 0; i < 8; i++) mp[i] = 0;
    for (size_t i = 0; i < hLen; i++) mp[8 + i] = hash[i];
    for (size_t i = 0; i < sLen; i++) mp[8 + hLen + i] = salt[i];
    sha256(mp, 8 + hLen + sLen, hp);
    return memcmp(hp, H, hLen) == 0 ? 0 : -1;
}
