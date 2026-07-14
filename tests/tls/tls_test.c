/*
 * TLS handshake-framing fuzz test (host-side, ASan/UBSan).
 *
 * kernel/tls.c parses UNTRUSTED bytes off the network with no stack guard page,
 * so an out-of-bounds read there is kernel corruption on a hostile/broken HTTPS
 * server. Every other untrusted parser in the tree already has an adversarial
 * fuzz harness (x509/json/regex/deflate/zip/png/webp/html/url/bignum) -- TLS
 * record + handshake framing was the one gap. This closes it.
 *
 * We reach tls.c's *static* framing parsers by #include-ing the real source (the
 * same technique as tests/crypto/bignum_fuzz_test.c and tests/url/url_test.c),
 * so this fuzzes the ACTUAL kernel code, not a copy. Two parsers are exercised:
 *   - read_record()          the record layer: 5-byte header, length field,
 *                            bounded copy into the caller's buffer.
 *   - tls_capture_leaf_key()  the Certificate message: a nested walk of
 *                            certificate_request_context / cert-list-length /
 *                            per-cert length / per-cert extensions -- all
 *                            attacker-controlled 2- and 3-byte length fields.
 *
 * The crypto/net/kernel functions tls.c depends on are either stubbed here (the
 * network + a few kernel services) or linked from the real crypto .c files (see
 * run-tls-tests.sh). None of the crypto path actually runs: smp_parallel_for is
 * a no-op so the chain-signature verification is skipped, and we never drive the
 * encrypted-record path -- we only feed bytes to the two framing parsers.
 *
 * A clean exit = pass; any OOB/overflow aborts under ASan; a non-terminating
 * parser hangs the run. Run via tests/run-tls-tests.sh ("make tlsfuzztest").
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Pull in the types the stubs below reference (tcp_conn, struct rtc_time,
 * smp_fn). tls.c re-includes these (header guards make that a no-op). */
#include "net.h"
#include "rtc.h"
#include "smp.h"

/* ---- stubs for the kernel/network services tls.c links against ---- *
 * The crypto (sha256/hkdf/aesgcm/chachapoly/x25519/rsa/ecdsa/x509/bignum) is the
 * REAL code, linked in run-tls-tests.sh. Only the network + a few kernel hooks
 * are faked, because the framing parsers we target never touch the wire or the
 * signature math (smp_parallel_for is a no-op, so the chain verify is skipped). */
void kprintf(const char *fmt, ...) { (void)fmt; }
void rtc_now(struct rtc_time *t) { t->year = 2000; t->month = 1; t->day = 1; t->hour = 0; t->min = 0; t->sec = 0; }
void smp_parallel_for(int n, smp_fn fn, void *ctx) { (void)n; (void)fn; (void)ctx; }
int  tcp_read(tcp_conn *c, uint8_t *out, int max, uint64_t ticks) { (void)c; (void)out; (void)max; (void)ticks; return -1; }
int  tcp_write(tcp_conn *c, const uint8_t *data, int len) { (void)c; (void)data; return len; }
int  tcp_connect(tcp_conn *c, const uint8_t ip[4], uint16_t port) { (void)c; (void)ip; (void)port; return 0; }
void tcp_close(tcp_conn *c) { (void)c; }
int  dns_resolve(const char *host, uint8_t out_ip[4]) { (void)host; (void)out_ip; return -1; }
int  parse_ipv4(const char *s, uint8_t out[4]) { (void)s; (void)out; return -1; }   /* tls.c calls it before dns (M1847); no IP-literal fuzz input, so a stub suffices */
/* ecdsa.c's per-core Barrett-context migration guard (never exercised here) */
int  smp_current_cpu(void) { return 0; }
int  task_pin_here(void) { return 0; }
void task_unpin(int saved_pin_core) { (void)saved_pin_core; }

/* the parsers under test are static -- include the real source to reach them */
#include "../../kernel/tls.c"

static int fails, checks;

static uint32_t rs = 0xC0FFEEu;                  /* deterministic xorshift32 */
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

/* ---- read_record: feed a fully-buffered record + assert bounded behaviour ---- */
static void feed_record(const uint8_t *bytes, int n, int maxn) {
    tls t;
    for (int i = 0; i < (int)sizeof(t.rbuf); i++) t.rbuf[i] = 0;
    int cap = n; if (cap > (int)sizeof(t.rbuf)) cap = (int)sizeof(t.rbuf);
    for (int i = 0; i < cap; i++) t.rbuf[i] = bytes[i];
    t.rhead = 0; t.rtail = cap; t.tcp = 0;
    uint8_t out[20000];
    int type, outlen;
    checks++;
    int rc = read_record(&t, &type, out, maxn, &outlen);
    if (rc == 0) {
        /* success contract: 0 <= outlen <= maxn (and <= 18432, the record cap) */
        if (outlen < 0 || outlen > maxn || outlen > 18432) {
            printf("  FAIL read_record: rc=0 but outlen=%d (maxn=%d)\n", outlen, maxn);
            fails++;
        }
    } else if (rc != -1) {
        printf("  FAIL read_record: rc=%d (expected 0 or -1)\n", rc);
        fails++;
    }
}

/* ---- tls_capture_leaf_key: feed a Certificate-message body of any shape ---- */
static void feed_certmsg(const uint8_t *m, int mlen) {
    tls t;
    t.leaf_key_len = 0; t.host_ok = -2; t.cert_time_ok = -2; t.tcp = 0;
    checks++;
    int rc = tls_capture_leaf_key(&t, m, mlen, "example.com");
    if (rc != 0 && rc != -1) {
        printf("  FAIL tls_capture_leaf_key: rc=%d (expected 0 or -1)\n", rc);
        fails++;
    }
    /* if it claimed a leaf key, the copied length must fit the struct field */
    if (t.leaf_key_len < 0 || t.leaf_key_len > (int)sizeof(t.leaf_key)) {
        printf("  FAIL tls_capture_leaf_key: leaf_key_len=%d out of range\n", t.leaf_key_len);
        fails++;
    }
}

/* Build a structurally valid Certificate message: ctx_len(1)=0, list_len(3),
 * then one entry { cert_len(3), cert[cert_len], ext_len(2)=0 }. The cert bytes
 * are arbitrary (x509_parse rejects them, which is fine -- the framing WALK, the
 * OOB-prone part, still executes fully). Returns the byte count. */
static int build_certmsg(uint8_t *m, int certlen) {
    int p = 0;
    m[p++] = 0;                                  /* certificate_request_context length 0 */
    int listlen = 3 + certlen + 2;               /* cert_len(3) + cert + ext_len(2) */
    m[p++] = (listlen >> 16) & 0xFF; m[p++] = (listlen >> 8) & 0xFF; m[p++] = listlen & 0xFF;
    m[p++] = (certlen >> 16) & 0xFF; m[p++] = (certlen >> 8) & 0xFF; m[p++] = certlen & 0xFF;
    for (int i = 0; i < certlen; i++) m[p++] = (uint8_t)(0x30 + i);   /* junk cert bytes */
    m[p++] = 0; m[p++] = 0;                      /* extensions length 0 */
    return p;
}

int main(void) {
    printf("TLS framing fuzz (read_record + tls_capture_leaf_key)\n");

    /* ---- directed cases (known-answer) ---- */
    {   /* a valid ApplicationData record round-trips with the right length */
        uint8_t r[64] = { 23, 0x03, 0x03, 0x00, 0x08, 1,2,3,4,5,6,7,8 };
        tls t; t.rhead = 0; t.rtail = 13; t.tcp = 0;
        for (int i = 0; i < 13; i++) t.rbuf[i] = r[i];
        uint8_t out[64]; int type, outlen;
        checks++;
        if (read_record(&t, &type, out, sizeof(out), &outlen) != 0 || type != 23 || outlen != 8 ||
            out[0] != 1 || out[7] != 8) { printf("  FAIL: valid record did not round-trip\n"); fails++; }
    }
    {   /* a record claiming more than the caller's buffer must be rejected */
        uint8_t r[8] = { 23, 0x03, 0x03, 0xFF, 0xFF };
        feed_record(r, 5, 16);                   /* len=65535 > maxn=16 -> reject */
    }
    {   /* Certificate framing at exact boundaries + one-past must reject cleanly */
        uint8_t m[512]; int n = build_certmsg(m, 40);
        feed_certmsg(m, n);                       /* well-formed framing */
        feed_certmsg(m, n - 1);                   /* truncated last byte */
        feed_certmsg(m, 4);                       /* only ctx+partial list length */
        feed_certmsg(m, 1);                       /* only ctx length */
        feed_certmsg(m, 0);                       /* empty */
    }

    /* ---- fuzz: mutate a valid Certificate message ---- */
    uint8_t base[4096]; int baselen = build_certmsg(base, 200);
    for (int it = 0; it < 300000; it++) {
        uint8_t m[4096];
        int n = baselen;
        for (int i = 0; i < n; i++) m[i] = base[i];
        int muts = 1 + (xr() % 6);
        for (int k = 0; k < muts; k++) {
            int pos = xr() % n;
            uint32_t op = xr() % 4;
            if      (op == 0) m[pos] = (uint8_t)xr();          /* random byte */
            else if (op == 1) m[pos] ^= (uint8_t)(1 << (xr() & 7)); /* bit flip */
            else if (op == 2) m[pos] = 0xFF;                   /* max out a length byte */
            else              n = 1 + (xr() % n);              /* truncate */
        }
        feed_certmsg(m, n);
    }

    /* ---- fuzz: mutate a valid record header + payload ---- */
    for (int it = 0; it < 300000; it++) {
        uint8_t r[600];
        int n = 5 + (xr() % 500);
        for (int i = 0; i < n; i++) r[i] = (uint8_t)xr();
        r[0] = (uint8_t)(20 + (xr() % 5));                     /* a plausible record type */
        feed_record(r, n, (int)(xr() % 20001));                /* random caller buffer size */
    }

    /* ---- fuzz: pure random bytes into both parsers (no valid prefix) ---- */
    for (int it = 0; it < 200000; it++) {
        uint8_t b[300];
        int n = xr() % (int)sizeof(b);
        for (int i = 0; i < n; i++) b[i] = (uint8_t)xr();
        feed_certmsg(b, n);
        feed_record(b, n, (int)(xr() % 20001));
    }

    if (fails) { printf("FAIL: %d/%d checks failed\n", fails, checks); return 1; }
    printf("PASS: %d checks, TLS record + cert-chain framing OOB-clean under fuzz\n", checks);
    return 0;
}
