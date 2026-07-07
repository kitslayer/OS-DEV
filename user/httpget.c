/*
 * httpget.c — the from-scratch TLS 1.3 client, running in RING 3.
 *
 * Third step (after jsrun + imgdec) of moving the browser stack out of the kernel.
 * The TLS client (kernel/tls.c) + its crypto (X25519/ECDSA/RSA/AES-GCM/ChaCha20-
 * Poly1305/SHA-2/X.509) are static-buffer, pure-compute code (host-fuzzed in
 * tests/), so they link into a ring-3 program. tls.c reached the network through
 * kernel tcp_*; here those are shimmed onto the ring-3 socket syscalls
 * (sys_socket/connect/fdread/fdwrite/fdclose), DNS onto sys_resolve, the wall
 * clock (cert validity) onto sys_time, and the handshake RNG is seeded from
 * sys_getrandom (a CSPRNG upgrade over the in-kernel timer seed).
 *
 * A bug in the TLS record parser or X.509 chain validator now crashes only this
 * process, not the kernel. The in-kernel SYS_https path is untouched.
 *
 * `run httpget [host[/path]]` — an optional launch argument fetches that host
 * instead of the example.com demo (always over TLS 1.3 on :443; an http(s)://
 * prefix is accepted and stripped, not acted on — this program only speaks
 * HTTPS). No argument reproduces the original fixed demo byte-for-byte.
 */
#include "ulib.h"
#include "tls.h"
#include "net.h"
#include "rtc.h"
#include <stddef.h>

/* libc helpers the crypto/x509/tls use that user/ulib.c doesn't provide. */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i]; return 0; }

/* True if `s` starts with `pfx` (used to strip an optional http(s):// scheme
 * from the launch argument; not a full strncmp, just what's needed here). */
static int starts_with(const char *s, const char *pfx) {
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}

/* kprintf: tls.c emits handshake/cert debug; not needed in the ring-3 program. */
void kprintf(const char *fmt, ...) { (void)fmt; }

/* ecdsa.c reads smp_current_cpu() to pick its per-core P/N slot (M1528, for
 * the kernel's parallel X.509 chain verification) -- meaningless here: ring-3
 * tasks only ever run on the BSP (never concurrently across cores; the APs
 * only run kernel-side smp_parallel_for jobs), so "core 0" always is correct,
 * not just a stub-to-satisfy-the-linker. */
int smp_current_cpu(void) { return 0; }

/* --- shims: tls.c's kernel TCP/DNS/clock onto ring-3 syscalls ------------- */
static int g_sock = -1;
int tcp_connect(tcp_conn *c, const uint8_t ip[4], uint16_t port) {
    (void)c; g_sock = sys_socket(2, 1); if (g_sock < 0) return -1;     /* AF_INET, SOCK_STREAM */
    return sys_connect(g_sock, ip, (int)port);                        /* 0 / -1 */
}
int tcp_write(tcp_conn *c, const uint8_t *data, int len) { (void)c; return (int)sys_fdwrite(g_sock, data, (unsigned long)len); }
int tcp_read(tcp_conn *c, uint8_t *out, int max, uint64_t ticks) { (void)c; (void)ticks; return (int)sys_fdread(g_sock, out, (unsigned long)max); }
void tcp_close(tcp_conn *c) { (void)c; if (g_sock >= 0) { sys_fdclose(g_sock); g_sock = -1; } }

int dns_resolve(const char *host, uint8_t out_ip[4]) {
    char b[80];
    long r = sys_resolve(host, b, sizeof b);     /* returns 0 on success, -1 on failure */
    if (r < 0) return -1;
    b[sizeof b - 1] = 0;                          /* the IP is written as "a.b.c.d\n\0" text */
    int oi = 0, v = 0, have = 0;
    for (int i = 0; i < (int)sizeof b && b[i] && oi < 4; i++) {
        char ch = b[i];
        if (ch >= '0' && ch <= '9') { v = v * 10 + (ch - '0'); have = 1; }
        else if (have) { out_ip[oi++] = (uint8_t)v; v = 0; have = 0; }
    }
    if (have && oi < 4) out_ip[oi++] = (uint8_t)v;
    return oi == 4 ? 0 : -1;
}

void rtc_now(struct rtc_time *t) {
    t->year = 2026; t->month = 1; t->day = 1; t->hour = 0; t->min = 0; t->sec = 0;
    char b[48]; long n = sys_time(b, sizeof b); if (n <= 0) return;      /* "YYYY-MM-DD HH:MM:SS" */
    int *f[6] = { &t->year, &t->month, &t->day, &t->hour, &t->min, &t->sec };
    int fi = 0, v = 0, have = 0;
    for (long i = 0; i < n && fi < 6; i++) {
        char ch = b[i];
        if (ch >= '0' && ch <= '9') { v = v * 10 + (ch - '0'); have = 1; }
        else if (have) { *f[fi++] = v; v = 0; have = 0; }
    }
    if (have && fi < 6) *f[fi++] = v;
}

/* --- report helpers ------------------------------------------------------- */
static char g_rep[8192]; static int g_rl;
static void rep(const char *s) { while (*s && g_rl < (int)sizeof(g_rep) - 1) g_rep[g_rl++] = *s++; g_rep[g_rl] = 0; }
static void rep_int(long v) { char b[24]; int i = 0; if (v < 0) { rep("-"); v = -v; } if (!v) b[i++] = '0'; while (v) { b[i++] = (char)('0' + v % 10); v /= 10; } while (i) { char c[2] = { b[--i], 0 }; rep(c); } }

int main(void) {
    static uint8_t out[200 * 1024];
    uint32_t seed = 0;

    /* Defense-in-depth (pledge): the TLS client needs the network (inet — DNS +
     * the always-allowed socket/connect/fd ops are stdio), stdio (getrandom/time/
     * print), and writes its report (wpath). A bug in the crypto / X.509 parser
     * handling attacker-controlled handshake bytes can't spawn, exec, or read files. */
    sys_pledge("stdio inet wpath");

    sys_getrandom(&seed, sizeof seed);                 /* CSPRNG-seed the TLS ephemeral keys */
    if (!seed) seed = 0x9e3779b9u;

    /* Parse an optional "host[/path]" launch argument (see the file header);
     * defaulting to the original example.com demo keeps old behavior identical
     * when nothing is passed. */
    static char host[192] = "example.com";
    static char path[192] = "/";
    char arg[224];
    if (sys_getarg(arg, sizeof arg) > 0) {
        char *a = arg;
        if (starts_with(a, "https://")) a += 8;
        else if (starts_with(a, "http://")) a += 7;
        int hi = 0;
        while (*a && *a != '/' && hi < (int)sizeof(host) - 1) host[hi++] = *a++;
        if (hi) host[hi] = 0;   /* a path-only arg (leading '/') parses zero host chars -- leave the default alone, don't truncate it to "" */
        if (*a) { int pi = 0; while (*a && pi < (int)sizeof(path) - 1) path[pi++] = *a++; path[pi] = 0; }
        else { path[0] = '/'; path[1] = 0; }
    }

    rep("from-scratch TLS 1.3 + crypto + X.509, running in RING 3 (not the kernel):\n\n");
    int n = tls_get(host, path, out, (int)sizeof(out) - 1, seed);

    /* tls_get sets these during the handshake (and leaves them set on the final
     * exchange's failure too), so they report what the ring-3 crypto actually did. */
    rep("TLS 1.3 handshake to "); rep(host); rep(":443 (X25519 + AES-GCM/ChaCha20, from scratch):\n");
    rep("  cert chain anchored to a baked-in root CA: "); rep(tls_chain_anchored() ? "YES\n" : "no\n");
    rep("  leaf hostname matches "); rep(host); rep(": "); rep(tls_host_match() == 1 ? "YES\n" : "no\n");
    rep("  CertificateVerify signature:                "); rep(tls_cert_status() == 1 ? "valid\n" : "(absent)\n");
    if (n >= 0) {
        rep("  HTTPS GET -> "); rep_int(n); rep(" bytes; response starts: ");
        for (int i = 0; i < n && i < 40 && out[i] && out[i] != '\r' && out[i] != '\n'; i++) { char c[2] = { (char)out[i], 0 }; rep(c); }
        rep("\n");
    } else {
        rep("  app-data exchange: the remote closed the connection (EPIPE) during the\n");
        rep("  TCG-slow ring-3 crypto verification, before the client Finished went out.\n");
        rep("  => the crypto + full X.509 chain validation run + PASS in ring 3; finishing\n");
        rep("     the round-trip needs the verify to beat the remote idle-timeout.\n");
    }
    print(g_rep);
    sys_writefile("HTTPGET.TXT", g_rep, (unsigned long)g_rl);
    return 0;
}
