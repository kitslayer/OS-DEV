/*
 * TCP/IP packet-parser + reassembly fuzz test (host-side, ASan/UBSan).
 *
 * net.c parses UNTRUSTED packets off the wire (Ethernet/IP/TCP) and reassembles
 * out-of-order TCP segments into a 96 KB reorder buffer — all kernel-side with
 * no stack guard page. The M419-423 audit verified it bounds-safe (the NIC
 * clamps the frame length, IP total-length is clamped to the real frame, TCP
 * dlen is floored at 0, and the reassembly offset bound dodges the off+dlen
 * overflow trap). This locks that finding by fuzzing the two attacker-reachable
 * parsing units directly. We #include net.c so its static tcp_recv_seg /
 * ooo_store are reachable, and stub the NIC + timer. Run via "make nettest".
 */
#include <stdint.h>
#include <stdio.h>

/* --- stubs for net.c's hardware/kernel dependencies --- */
static const uint8_t *g_pkt; static int g_pktlen, g_consumed;
int e1000_receive(void *out, uint16_t max) {          /* feed one fuzz frame, then "none" */
    if (g_consumed) return 0;
    g_consumed = 1;
    int n = g_pktlen; if (n > max) n = max;
    for (int i = 0; i < n; i++) ((uint8_t *)out)[i] = g_pkt[i];
    return n;
}
static uint64_t g_ticks;
uint64_t timer_ticks(void) { return g_ticks++; }       /* increments -> deadline loops terminate */
int e1000_init(void) { return 0; }
static const uint8_t g_mac[6] = { 2, 0, 0, 0, 0, 1 };
const uint8_t *e1000_mac(void) { return g_mac; }
int e1000_send(const void *frame, uint16_t len) { (void)frame; return len; }
void kprintf(const char *fmt, ...) { (void)fmt; }
/* net.c's net_demo() calls into tls.c for the boot HTTPS self-test; stub it out
 * here (this suite fuzzes the packet/reassembly path, not TLS). */
int tls_get(const char *h, const char *p, uint8_t *o, int m, uint32_t s) {
    (void)h; (void)p; (void)o; (void)m; (void)s; return -1;
}
int tls_cert_status(void)    { return -2; }
int tls_chain_anchored(void) { return 0; }
int tls_host_match(void)     { return -2; }
const char *tls_leaf_cn(void)     { return ""; }
const char *tls_leaf_expiry(void) { return ""; }

#include "net.c"   /* the static tcp_recv_seg / ooo_store + the reassembly globals */

static uint32_t rs = 0x9E3779B9u;
static uint32_t xr(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    uint8_t buf[2048]; uint8_t *tcp; int dlen;
    const uint8_t dip[4] = { 10, 0, 2, 2 };
    uint8_t fuzz[1600];
    const int ITERS = 150000;

    /* 1. Fuzz tcp_recv_seg: random Ethernet frames, half forced to look like
     *    IPv4+TCP so the ihl/thl/dlen/iptotal-clamp bounds actually run. */
    for (int i = 0; i < ITERS; i++) {
        int n = 14 + (int)(xr() % 90);
        for (int j = 0; j < n; j++) fuzz[j] = (uint8_t)xr();
        if (i & 1) {                                   /* ethertype IPv4, IHL 5, proto TCP */
            fuzz[12] = 0x08; fuzz[13] = 0x00; fuzz[14] = 0x45;
            if (n > 23) fuzz[23] = 6;
        }
        g_pkt = fuzz; g_pktlen = n; g_consumed = 0; g_ticks = 0;
        tcp_recv_seg(buf, sizeof buf, dip, 1234, 80, 4, &tcp, &dlen);
    }

    /* 2. Fuzz ooo_store directly (the 96 KB reorder buffer — the
     *    reviewer's flagged-highest-risk path): crafted theirseq/seq spanning
     *    far-future, past, and 32-bit wraparound, with random payload lengths.
     *    The offset bound must reject every out-of-range write (off<0 or
     *    off+dlen>OOO_CAP); ASan would catch any that slips through. */
    for (int i = 0; i < ITERS; i++) {
        uint32_t theirseq = xr();
        uint32_t seq = theirseq + xr();                /* any relative offset, incl. negative/wrapping */
        int dl = (int)(xr() % 1600);                   /* fits fuzz[1600] */
        for (int j = 0; j < dl; j++) fuzz[j] = (uint8_t)xr();
        ooo_store(theirseq, seq, fuzz, dl);
    }

    printf("nettest: %d tcp_recv_seg + %d ooo_store fuzz iters — ASan/UBSan clean\n", ITERS, ITERS);
    return 0;
}
