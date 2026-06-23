/*
 * net.c — a tiny taste of a network stack: ARP + ICMP echo (ping).
 *
 * This is the protocol layer on top of the NIC. It's card-agnostic: it builds
 * raw packets byte-by-byte in network (big-endian) order and hands them to
 * whichever card the nic.c dispatcher bound (the Intel e1000 or the Realtek
 * RTL8139), via nic_send / nic_receive / nic_mac. Then it polls for replies.
 * Two exchanges:
 *
 *   ARP  — "who has 10.0.2.2? tell 10.0.2.15" → the gateway answers with its
 *          MAC. (You must know a host's MAC before you can send it IP packets.)
 *   ICMP — an echo request ("ping") to the gateway → it echoes it back.
 *
 * Under QEMU's user-mode networking, the virtual gateway 10.0.2.2 answers both.
 */
#include "net.h"
#include "nic.h"
#include "timer.h"
#include "console.h"
#include "string.h"
#include "tls.h"
#include "rtc.h"
#include <stdint.h>

/* The SLIRP defaults — used as-is until a DHCP lease (net_dhcp) overwrites them. */
static uint8_t  OUR_IP[4]  = {10, 0, 2, 15};
static uint8_t  GW_IP[4]   = {10, 0, 2, 2};
static const uint8_t  BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

const uint8_t *net_ip(void)      { return OUR_IP; }
const uint8_t *net_gateway(void) { return GW_IP; }
const uint8_t *net_mac(void)     { return nic_mac(); }

/* --- byte helpers (everything on the wire is big-endian) --- */
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static uint16_t get16(const uint8_t *p)   { return (uint16_t)(p[0] << 8 | p[1]); }

/* Internet checksum: one's-complement sum of 16-bit big-endian words. */
static uint16_t inet_checksum(const uint8_t *data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(data[i] << 8 | data[i + 1]);
    if (len & 1)
        sum += (uint32_t)(data[len - 1] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Wait up to `ticks` for a frame; return its length (0 on timeout). */
static int recv_timeout(uint8_t *buf, int max, uint64_t ticks) {
    uint64_t deadline = timer_ticks() + ticks;
    while (timer_ticks() < deadline) {
        int len = nic_receive(buf, max);
        if (len > 0)
            return len;
    }
    return 0;
}

static void print_mac(const uint8_t *m) {
    for (int i = 0; i < 6; i++)
        kprintf("%s%x", i ? ":" : "", m[i]);
}

/* Tiny ARP cache: the gateway/DNS MAC is otherwise re-resolved (a broadcast +
 * reply round-trip) on every TCP connection. Cache it, with a TTL so a changed
 * mapping can't be served forever. A miss/expiry just falls through to a request. */
#define ARP_CACHE_N 4
#define ARP_TTL     6000           /* 60 s at 100 Hz */
static struct { uint8_t ip[4], mac[6]; uint64_t exp; int used; } arp_cache[ARP_CACHE_N];

/* Resolve `ip` to a MAC via ARP. Returns 1 + fills `out_mac`, or 0 on timeout. */
static int arp_resolve(const uint8_t *ip, uint8_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_N; i++)          /* serve a fresh cached mapping */
        if (arp_cache[i].used && timer_ticks() < arp_cache[i].exp
            && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            memcpy(out_mac, arp_cache[i].mac, 6);
            return 1;
        }
    const uint8_t *mac = nic_mac();
    uint8_t pkt[42];

    /* ethernet header */
    memcpy(pkt + 0, BROADCAST, 6);
    memcpy(pkt + 6, mac, 6);
    put16(pkt + 12, 0x0806);                 /* ethertype = ARP */
    /* arp body */
    put16(pkt + 14, 1);                      /* htype = ethernet */
    put16(pkt + 16, 0x0800);                 /* ptype = IPv4 */
    pkt[18] = 6; pkt[19] = 4;                /* hlen, plen */
    put16(pkt + 20, 1);                      /* oper = request */
    memcpy(pkt + 22, mac, 6);                /* sender MAC */
    memcpy(pkt + 28, OUR_IP, 4);             /* sender IP */
    memset(pkt + 32, 0, 6);                  /* target MAC (unknown) */
    memcpy(pkt + 38, ip, 4);                 /* target IP */
    nic_send(pkt, sizeof(pkt));

    /* Deadline-bounded, not a fixed try count: after a large transfer the RX ring
     * is full of stale TCP packets that recv_timeout returns instantly, so a fixed
     * budget would be burned skipping them before the ARP reply arrives. Keep
     * draining-and-matching until the deadline instead. */
    uint8_t buf[1600];
    uint64_t deadline = timer_ticks() + 200;
    while (timer_ticks() < deadline) {
        int len = recv_timeout(buf, sizeof(buf), 20);
        if (len >= 42 && get16(buf + 12) == 0x0806 && get16(buf + 20) == 2 /*reply*/
            && memcmp(buf + 28, ip, 4) == 0) {
            memcpy(out_mac, buf + 22, 6);    /* sender MAC of the reply */
            static int rr = 0;               /* cache it (round-robin evict) */
            int slot = -1;
            for (int i = 0; i < ARP_CACHE_N; i++)
                if (arp_cache[i].used && memcmp(arp_cache[i].ip, ip, 4) == 0) { slot = i; break; }
            if (slot < 0) { slot = rr; rr = (rr + 1) % ARP_CACHE_N; }
            memcpy(arp_cache[slot].ip, ip, 4); memcpy(arp_cache[slot].mac, out_mac, 6);
            arp_cache[slot].exp = timer_ticks() + ARP_TTL; arp_cache[slot].used = 1;
            return 1;
        }
    }
    return 0;
}

/* Send one ICMP echo request to `ip` (via `dst_mac`) and await the reply. */
static int ping(const uint8_t *ip, const uint8_t *dst_mac, uint16_t seq) {
    const uint8_t *mac = nic_mac();
    uint8_t pkt[42];                         /* 14 eth + 20 IP + 8 ICMP */

    /* ethernet */
    memcpy(pkt + 0, dst_mac, 6);
    memcpy(pkt + 6, mac, 6);
    put16(pkt + 12, 0x0800);                 /* IPv4 */

    /* IP header (20 bytes, at offset 14) */
    uint8_t *ip_h = pkt + 14;
    ip_h[0] = 0x45;                          /* version 4, IHL 5 */
    ip_h[1] = 0;                             /* DSCP/ECN */
    put16(ip_h + 2, 28);                     /* total length = 20 + 8 */
    put16(ip_h + 4, seq);                    /* identification */
    put16(ip_h + 6, 0);                      /* flags/fragment */
    ip_h[8] = 64;                            /* TTL */
    ip_h[9] = 1;                             /* protocol = ICMP */
    put16(ip_h + 10, 0);                     /* checksum (fill below) */
    memcpy(ip_h + 12, OUR_IP, 4);
    memcpy(ip_h + 16, ip, 4);
    put16(ip_h + 10, inet_checksum(ip_h, 20));

    /* ICMP header (8 bytes, at offset 34) */
    uint8_t *icmp = pkt + 34;
    icmp[0] = 8;                             /* type = echo request */
    icmp[1] = 0;                             /* code */
    put16(icmp + 2, 0);                      /* checksum (fill below) */
    put16(icmp + 4, 0x1234);                 /* identifier */
    put16(icmp + 6, seq);                    /* sequence */
    put16(icmp + 2, inet_checksum(icmp, 8));

    nic_send(pkt, sizeof(pkt));

    uint8_t buf[1600];
    uint64_t deadline = timer_ticks() + 200;   /* deadline-bounded (see arp_resolve) */
    while (timer_ticks() < deadline) {
        int len = recv_timeout(buf, sizeof(buf), 20);
        if (len >= 42 && get16(buf + 12) == 0x0800 && buf[14 + 9] == 1 /*ICMP*/
            && buf[34] == 0 /*echo reply*/ && memcmp(buf + 26, ip, 4) == 0) {
            return 1;
        }
    }
    return 0;
}

static uint8_t DNS_IP[4] = {10, 0, 2, 3};       /* SLIRP default; a DHCP lease may replace it */
const uint8_t *net_dns(void)     { return DNS_IP; }

int net_ping_gateway(void) {
    uint8_t mac[6];
    if (!arp_resolve(GW_IP, mac))
        return -1;
    int got = 0;
    for (uint16_t s = 1; s <= 3; s++)
        if (ping(GW_IP, mac, s))
            got++;
    return got;
}

/* Ping a host by name: DNS-resolve it, then ICMP-echo the address 3 times,
 * routed through the gateway (the next hop for any off-LAN destination — the
 * same ping() helper, just a remote target IP behind the gateway's MAC).
 * Returns the echo-reply count (0-3), or -1 if DNS or the gateway ARP fails. */
int net_ping_host(const char *host) {
    uint8_t ip[4];
    if (dns_resolve(host, ip) != 0) return -1;
    uint8_t mac[6];
    if (!arp_resolve(GW_IP, mac)) return -1;     /* route via the gateway */
    int got = 0;
    for (uint16_t s = 1; s <= 3; s++)
        if (ping(ip, mac, s))
            got++;
    return got;
}

/* Tiny DNS cache: browsing a site re-resolves the same host on every page/link
 * fetch, so cache recent answers. Entries expire after DNS_TTL ticks so a stale
 * IP is never served indefinitely; a cache miss just falls through to a query. */
#define DNS_CACHE_N 8
#define DNS_TTL     6000           /* 60 s at 100 Hz */
static struct { char host[64]; uint8_t ip[4]; uint64_t exp; int used; } dns_cache[DNS_CACHE_N];
static int dns_streq(const char *a, const char *b) {
    int i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];           /* equal only if both ended together */
}
static void dns_cache_put(const char *host, const uint8_t ip[4]) {
    int j = 0; while (host[j] && j < 63) j++;
    if (host[j]) return;           /* name too long to cache cleanly: skip */
    static int rr = 0;
    int slot = -1;
    for (int i = 0; i < DNS_CACHE_N; i++)
        if (dns_cache[i].used && dns_streq(dns_cache[i].host, host)) { slot = i; break; }
    if (slot < 0) { slot = rr; rr = (rr + 1) % DNS_CACHE_N; }   /* round-robin evict */
    for (int k = 0; k <= j; k++) dns_cache[slot].host[k] = host[k];
    memcpy(dns_cache[slot].ip, ip, 4);
    dns_cache[slot].exp = timer_ticks() + DNS_TTL;
    dns_cache[slot].used = 1;
}

/* Minimal DNS A-record lookup over UDP, to the SLIRP resolver at 10.0.2.3. */
int dns_resolve(const char *host, uint8_t out_ip[4]) {
    for (int i = 0; i < DNS_CACHE_N; i++)          /* serve a fresh cached answer */
        if (dns_cache[i].used && timer_ticks() < dns_cache[i].exp
            && dns_streq(dns_cache[i].host, host)) {
            memcpy(out_ip, dns_cache[i].ip, 4);
            return 0;
        }
    uint8_t mac[6];
    if (!arp_resolve(DNS_IP, mac))
        return -1;
    const uint8_t *me = nic_mac();

    /* build the DNS query payload */
    uint8_t q[256]; int dl = 0;
    put16(q + 0, 0x2A2A);          /* id */
    put16(q + 2, 0x0100);          /* flags: recursion desired */
    put16(q + 4, 1); put16(q + 6, 0); put16(q + 8, 0); put16(q + 10, 0);
    dl = 12;
    const char *p = host;
    while (*p) {
        int len = 0; while (p[len] && p[len] != '.' && len < 64) len++;  /* bound the read; a DNS label is max 63 bytes */
        if (len > 63 || dl + 1 + len > (int)sizeof(q) - 5) return -1;     /* reject a too-long label or one that would overflow q[] (leave room for root + QTYPE + QCLASS) */
        q[dl++] = (uint8_t)len;
        for (int i = 0; i < len; i++) q[dl++] = p[i];
        p += len; if (*p == '.') p++;
    }
    q[dl++] = 0;                    /* root label */
    put16(q + dl, 1); dl += 2;      /* QTYPE A */
    put16(q + dl, 1); dl += 2;      /* QCLASS IN */

    /* wrap in Ethernet/IPv4/UDP */
    uint8_t pkt[400];
    memcpy(pkt + 0, mac, 6); memcpy(pkt + 6, me, 6); put16(pkt + 12, 0x0800);
    uint8_t *ip = pkt + 14, *udp = pkt + 34;
    ip[0] = 0x45; ip[1] = 0; put16(ip + 2, 20 + 8 + dl); put16(ip + 4, 0x42);
    put16(ip + 6, 0); ip[8] = 64; ip[9] = 17; put16(ip + 10, 0);
    memcpy(ip + 12, OUR_IP, 4); memcpy(ip + 16, DNS_IP, 4);
    put16(ip + 10, inet_checksum(ip, 20));
    put16(udp + 0, 5353); put16(udp + 2, 53); put16(udp + 4, 8 + dl); put16(udp + 6, 0);
    memcpy(udp + 8, q, dl);
    nic_send(pkt, 34 + 8 + dl);

    /* await + parse the response — deadline-bounded (see arp_resolve): a fixed try
     * count would be exhausted skipping stale TCP packets left in the RX ring by a
     * prior large transfer before the DNS reply (which we keep waiting for) lands. */
    uint8_t buf[1600];
    uint64_t deadline = timer_ticks() + 500;
    while (timer_ticks() < deadline) {
        int len = recv_timeout(buf, sizeof(buf), 20);
        if (len < 42 || get16(buf + 12) != 0x0800 || buf[14 + 9] != 17) continue;   /* IPv4/UDP */
        int ihl = (buf[14] & 0x0F) * 4;        /* honor the real IP header length (IP options -> IHL>5); SLIRP uses 20 so this is a no-op there, but real routers may not */
        int doff = 14 + ihl + 8;               /* eth(14) + IP(ihl) + UDP(8) -> DNS payload (was hardcoded 42 = IHL 5) */
        if (ihl < 20 || len < doff + 12) continue;
        uint8_t *d = buf + doff;               /* DNS payload */
        int dmax = len - doff;                 /* bytes available in d[] (adversarial) */
        if (dmax < 12 || get16(d) != 0x2A2A) continue;
        int an = get16(d + 6);
        if (an < 1) continue;
        /* every offset below is driven by the reply, so bound each against dmax
         * (a malformed name chain / rdlen must never index past the packet). */
        int o = 12;
        while (o < dmax && d[o]) { if ((d[o] & 0xC0) == 0xC0) { o++; break; } o += d[o] + 1; }
        o++; o += 4;                            /* end-of-name + QTYPE/QCLASS */
        for (int a = 0; a < an; a++) {
            if (o + 1 > dmax) break;
            if ((d[o] & 0xC0) == 0xC0) o += 2;
            else { while (o < dmax && d[o]) o += d[o] + 1; o++; }
            if (o + 10 > dmax) break;           /* type(2)+class(2)+ttl(4)+rdlen(2) */
            int type = get16(d + o); o += 2; o += 2; o += 4;   /* type,class,ttl */
            int rdlen = get16(d + o); o += 2;
            if (type == 1 && rdlen == 4 && o + 4 <= dmax) { memcpy(out_ip, d + o, 4); dns_cache_put(host, out_ip); return 0; }
            o += rdlen;
        }
    }
    return -1;
}

/* ===================================================================== *
 *  DHCP client — obtain {IP, gateway, DNS} from the server (DORA).
 *
 *  Replaces the hardcoded SLIRP defaults with a real lease. Built on the
 *  same byte-wrangling as dns_resolve, but broadcast from 0.0.0.0:68 to
 *  255.255.255.255:67 (we have no IP yet, so the reply must be broadcast —
 *  that's what the BOOTP `flags` broadcast bit requests). QEMU's SLIRP
 *  runs a real DHCP server, so this works end-to-end under emulation.
 * ===================================================================== */
static const uint8_t DHCP_MAGIC[4] = { 0x63, 0x82, 0x53, 0x63 };

/* Build + send one BOOTP/DHCP message. `mtype` is the DHCP message type
 * (1=DISCOVER, 3=REQUEST). For REQUEST, reqip/srvid carry options 50/54. */
static void dhcp_send(uint32_t xid, uint8_t mtype, const uint8_t *reqip, const uint8_t *srvid) {
    const uint8_t *me = nic_mac();
    uint8_t pkt[400]; memset(pkt, 0, sizeof pkt);
    memcpy(pkt + 0, BROADCAST, 6); memcpy(pkt + 6, me, 6); put16(pkt + 12, 0x0800);
    uint8_t *ip = pkt + 14, *udp = pkt + 34, *bp = pkt + 42;
    /* BOOTP fixed header */
    bp[0] = 1; bp[1] = 1; bp[2] = 6; bp[3] = 0;            /* op=BOOTREQUEST, htype=eth, hlen=6 */
    bp[4] = xid >> 24; bp[5] = xid >> 16; bp[6] = xid >> 8; bp[7] = (uint8_t)xid;
    put16(bp + 10, 0x8000);                                /* flags: broadcast (we have no IP to unicast to) */
    memcpy(bp + 28, me, 6);                                /* chaddr = our MAC */
    memcpy(bp + 236, DHCP_MAGIC, 4);
    int o = 240;
    bp[o++] = 53; bp[o++] = 1; bp[o++] = mtype;            /* opt 53: DHCP message type */
    if (mtype == 3) {                                      /* REQUEST echoes the offered IP + server id */
        if (reqip) { bp[o++] = 50; bp[o++] = 4; memcpy(bp + o, reqip, 4); o += 4; }
        if (srvid) { bp[o++] = 54; bp[o++] = 4; memcpy(bp + o, srvid, 4); o += 4; }
    }
    bp[o++] = 55; bp[o++] = 3; bp[o++] = 1; bp[o++] = 3; bp[o++] = 6;  /* opt 55: want subnet(1), router(3), dns(6) */
    bp[o++] = 0xFF;                                        /* end */
    int blen = o;
    put16(udp + 0, 68); put16(udp + 2, 67); put16(udp + 4, (uint16_t)(8 + blen)); put16(udp + 6, 0);  /* UDP cksum 0 = none */
    ip[0] = 0x45; ip[1] = 0; put16(ip + 2, (uint16_t)(20 + 8 + blen)); put16(ip + 4, 0); put16(ip + 6, 0);
    ip[8] = 64; ip[9] = 17; put16(ip + 10, 0);
    memset(ip + 12, 0x00, 4);                              /* src 0.0.0.0 */
    memset(ip + 16, 0xFF, 4);                              /* dst 255.255.255.255 */
    put16(ip + 10, inet_checksum(ip, 20));
    nic_send(pkt, 42 + blen);
}

/* Await a DHCP reply matching `xid`. Returns the DHCP message type (2=OFFER,
 * 5=ACK, 6=NAK), filling yiaddr + any present server-id/router/dns; else 0. */
static int dhcp_recv(uint32_t xid, uint64_t ticks, uint8_t *yiaddr,
                     uint8_t *srvid, uint8_t *router, uint8_t *dns) {
    uint8_t buf[1600];
    uint64_t deadline = timer_ticks() + ticks;
    while (timer_ticks() < deadline) {
        int len = recv_timeout(buf, sizeof buf, 20);
        if (len < 282) continue;                           /* eth14+ip20+udp8+bootp240 */
        if (get16(buf + 12) != 0x0800 || buf[14 + 9] != 17) continue;   /* IPv4 + UDP */
        int ihl = (buf[14] & 0x0F) * 4; if (ihl < 20) continue;
        uint8_t *udp = buf + 14 + ihl;
        if (get16(udp + 2) != 68) continue;                /* UDP dst port 68 (DHCP client) */
        uint8_t *bp = udp + 8;
        int boff = (int)(bp - buf);
        if (boff + 240 > len || bp[0] != 2) continue;      /* room for BOOTP + op=BOOTREPLY */
        uint32_t rxid = ((uint32_t)bp[4] << 24) | ((uint32_t)bp[5] << 16) | ((uint32_t)bp[6] << 8) | bp[7];
        if (rxid != xid || memcmp(bp + 236, DHCP_MAGIC, 4) != 0) continue;
        memcpy(yiaddr, bp + 16, 4);                        /* yiaddr = the IP being offered/leased to us */
        int mtype = 0, o = 240;
        while (boff + o < len && bp[o] != 0xFF) {          /* walk TLV options, bounded by the packet */
            if (bp[o] == 0) { o++; continue; }             /* pad */
            int t = bp[o++]; if (boff + o >= len) break;
            int l = bp[o++]; if (boff + o + l > len) break;
            if      (t == 53 && l == 1) mtype = bp[o];
            else if (t == 54 && l == 4 && srvid)  memcpy(srvid,  bp + o, 4);
            else if (t == 3  && l >= 4 && router) memcpy(router, bp + o, 4);
            else if (t == 6  && l >= 4 && dns)    memcpy(dns,    bp + o, 4);
            o += l;
        }
        if (mtype) return mtype;
    }
    return 0;
}

/* The DORA handshake. On success, commits the lease into OUR_IP/GW_IP/DNS_IP
 * and returns 0; on timeout/NAK returns -1 with the prior config left intact. */
int net_dhcp(void) {
    const uint8_t *me = nic_mac();
    uint32_t xid = 0x4f534400u ^ (uint32_t)timer_ticks()
                 ^ ((uint32_t)me[2] << 24) ^ ((uint32_t)me[3] << 16)
                 ^ ((uint32_t)me[4] << 8)  ^ me[5];
    uint8_t yiaddr[4] = {0}, srvid[4] = {0}, router[4] = {0}, dns[4] = {0};

    int got = 0;
    for (int t = 0; t < 4 && !got; t++) {                  /* DISCOVER -> OFFER */
        dhcp_send(xid, 1, 0, 0);
        if (dhcp_recv(xid, 150, yiaddr, srvid, router, dns) == 2) got = 1;
    }
    if (!got) return -1;

    got = 0;
    for (int t = 0; t < 4 && !got; t++) {                  /* REQUEST -> ACK */
        dhcp_send(xid, 3, yiaddr, srvid);
        int mt = dhcp_recv(xid, 150, yiaddr, srvid, router, dns);
        if (mt == 5) got = 1;
        else if (mt == 6) return -1;                       /* NAK */
    }
    if (!got) return -1;

    memcpy(OUR_IP, yiaddr, 4);                             /* commit the lease */
    if (router[0] | router[1] | router[2] | router[3]) memcpy(GW_IP, router, 4);
    if (dns[0] | dns[1] | dns[2] | dns[3])             memcpy(DNS_IP, dns, 4);
    return 0;
}

/* ===================================================================== *
 *  A general UDP datagram send + a TFTP client (RFC 1350).
 *
 *  udp_send_to wraps a payload in Ethernet/IPv4/UDP and ships it — the
 *  reusable datagram primitive DNS/DHCP open-coded. TFTP rides on it:
 *  RRQ -> the server streams DATA blocks, we ACK each. QEMU's SLIRP has a
 *  built-in TFTP server at 10.0.2.2 (`-netdev user,tftp=DIR`), so this is
 *  verifiable with no external infrastructure.
 * ===================================================================== */
static int parse_ipv4(const char *s, uint8_t out[4]) {
    int oct = 0, v = 0, any = 0;
    for (int i = 0; i < 4; i++) out[i] = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); any = 1; if (v > 255) return -1; }
        else if (*s == '.' || *s == 0) {
            if (!any || oct > 3) return -1;
            out[oct++] = (uint8_t)v; v = 0; any = 0;
            if (*s == 0) break;
        } else return -1;
    }
    return oct == 4 ? 0 : -1;
}

/* Send a UDP datagram (payload plen bytes) from sport to dstip:dport via dstmac. */
static void udp_send_to(const uint8_t *dstmac, const uint8_t *dstip,
                        uint16_t sport, uint16_t dport, const uint8_t *payload, int plen) {
    if (plen < 0 || plen > 1400) return;
    const uint8_t *me = nic_mac();
    uint8_t pkt[1500];
    memcpy(pkt + 0, dstmac, 6); memcpy(pkt + 6, me, 6); put16(pkt + 12, 0x0800);
    uint8_t *ip = pkt + 14, *udp = pkt + 34, *pl = pkt + 42;
    ip[0] = 0x45; ip[1] = 0; put16(ip + 2, (uint16_t)(20 + 8 + plen)); put16(ip + 4, 0); put16(ip + 6, 0);
    ip[8] = 64; ip[9] = 17; put16(ip + 10, 0);
    memcpy(ip + 12, OUR_IP, 4); memcpy(ip + 16, dstip, 4);
    put16(ip + 10, inet_checksum(ip, 20));
    put16(udp + 0, sport); put16(udp + 2, dport); put16(udp + 4, (uint16_t)(8 + plen)); put16(udp + 6, 0);
    for (int i = 0; i < plen; i++) pl[i] = payload[i];
    nic_send(pkt, 42 + plen);
}

/* Fetch `filename` from the TFTP server `server_str` (dotted-quad) into `out`
 * (capacity `max`). Returns the byte length, or -1. Lock-step RRQ/DATA/ACK;
 * latches the server's transfer port (TID) from its first DATA. */
long net_tftp_get(const char *server_str, const char *filename, void *out, uint32_t max) {
    uint8_t srv[4];
    if (parse_ipv4(server_str, srv) < 0) return -1;
    uint8_t mac[6];
    if (!arp_resolve(srv, mac) && !arp_resolve(GW_IP, mac)) return -1;   /* server, else via gateway */

    const uint16_t myport = 0x8200;            /* our client TID */
    uint16_t tid = 0;                          /* server's TID, learned from its first DATA */

    uint8_t rrq[256]; int rl = 0;              /* Read Request: 01 | filename | 0 | "octet" | 0 */
    rrq[rl++] = 0; rrq[rl++] = 1;
    for (const char *p = filename; *p && rl < 220; p++) rrq[rl++] = (uint8_t)*p;
    rrq[rl++] = 0;
    for (const char *p = "octet"; *p; p++) rrq[rl++] = (uint8_t)*p;
    rrq[rl++] = 0;
    udp_send_to(mac, srv, myport, 69, rrq, rl);

    long total = 0;
    uint16_t expect = 1;                       /* next DATA block we want */
    uint8_t buf[1600];
    int idle = 0;
    for (;;) {
        int len = recv_timeout(buf, sizeof buf, 100);   /* ~1 s */
        if (len <= 0) { if (++idle > 6) return -1; if (tid == 0) udp_send_to(mac, srv, myport, 69, rrq, rl); continue; }   /* no DATA yet: re-send RRQ; mid-transfer: await the server's retransmit */
        if (get16(buf + 12) != 0x0800 || buf[14 + 9] != 17) continue;   /* IPv4/UDP */
        int ihl = (buf[14] & 0x0F) * 4; if (ihl < 20) continue;
        uint8_t *udp = buf + 14 + ihl;
        if (get16(udp + 2) != myport) continue;          /* to our client port */
        int tlen = get16(udp + 4) - 8;                   /* TFTP payload length */
        if (tlen < 4 || 14 + ihl + 8 + tlen > len) continue;
        uint8_t *tp = udp + 8;
        uint16_t op = get16(tp);
        if (op == 5) return -1;                           /* ERROR packet */
        if (op != 3) continue;                            /* only DATA */
        uint16_t sport = get16(udp + 0);
        if (tid == 0) tid = sport;                        /* latch the server's transfer port */
        else if (sport != tid) continue;                  /* stray sender */
        uint16_t blk = get16(tp + 2);
        if (blk == expect) {
            int dlen = tlen - 4;
            if ((uint32_t)(total + dlen) > max) return -1;
            for (int i = 0; i < dlen; i++) ((uint8_t *)out)[total + i] = tp[4 + i];
            total += dlen;
            uint8_t ack[4] = { 0, 4, tp[2], tp[3] };      /* ACK this block */
            udp_send_to(mac, srv, myport, tid, ack, 4);
            expect++; idle = 0;
            if (dlen < 512) return total;                 /* a short block ends the transfer */
        } else {
            uint8_t ack[4] = { 0, 4, tp[2], tp[3] };      /* re-ACK a duplicate/old block */
            udp_send_to(mac, srv, myport, tid, ack, 4);
        }
    }
}

/* ===================================================================== *
 *  SNTP client (RFC 4330) — set the wall clock from a network time server.
 *  One 48-byte UDP packet to :123; the reply's transmit timestamp (seconds
 *  since 1900) at offset 40 converts to a civil date and is written to the RTC.
 * ===================================================================== */

/* Unix seconds (UTC) -> civil date, via Howard Hinnant's days_from_civil inverse. */
static void unix_to_rtc(uint64_t u, struct rtc_time *t) {
    t->sec = (int)(u % 60); u /= 60;
    t->min = (int)(u % 60); u /= 60;
    t->hour = (int)(u % 24); u /= 24;                /* u = days since 1970-01-01 */
    long z = (long)u + 719468;                       /* shift epoch to 0000-03-01 */
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                 /* day of era [0,146096] */
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);            /* day of year [0,365] */
    unsigned mp = (5*doy + 2)/153;                              /* month, Mar=0 */
    unsigned d  = doy - (153*mp + 2)/5 + 1;
    unsigned m  = mp < 10 ? mp + 3 : mp - 9;
    t->year = (int)(y + (m <= 2)); t->month = (int)m; t->day = (int)d;
}

/* Query pool.ntp.org and set the RTC. Returns 0 on success, -1 on failure
 * (no DNS, no route, or no reply within the timeout). */
int net_sntp(void) {
    uint8_t srv[4];
    if (dns_resolve("pool.ntp.org", srv) != 0) return -1;   /* resolve the server */
    uint8_t mac[6];
    if (!arp_resolve(GW_IP, mac)) return -1;                /* a public server routes via the gateway */

    const uint16_t myport = 0x8300;
    uint8_t pkt[48]; for (int i = 0; i < 48; i++) pkt[i] = 0;
    pkt[0] = 0x23;                                           /* LI=0, VN=4, Mode=3 (client) */
    udp_send_to(mac, srv, myport, 123, pkt, 48);

    uint8_t buf[1600];
    uint64_t deadline = timer_ticks() + 400;                /* ~4 s */
    while (timer_ticks() < deadline) {
        int len = recv_timeout(buf, sizeof buf, 50);
        if (len < 14 + 20 + 8 + 48) continue;
        if (get16(buf + 12) != 0x0800 || buf[14 + 9] != 17) continue;   /* IPv4/UDP */
        int ihl = (buf[14] & 0x0F) * 4; if (ihl < 20) continue;
        uint8_t *udp = buf + 14 + ihl;
        if (get16(udp + 2) != myport) continue;             /* to our client port */
        uint8_t *ntp = udp + 8;
        uint32_t secs = ((uint32_t)ntp[40] << 24) | ((uint32_t)ntp[41] << 16)
                      | ((uint32_t)ntp[42] << 8)  | (uint32_t)ntp[43];
        if (secs < 2208988800u) continue;                   /* before 1970 -> bogus */
        struct rtc_time t;
        unix_to_rtc((uint64_t)secs - 2208988800ull, &t);    /* NTP(1900) -> Unix(1970) */
        rtc_set(&t);
        return 0;
    }
    return -1;
}

/* ===================================================================== *
 *  Minimal TCP client + HTTP/1.0 GET.
 *
 *  Enough TCP to open a connection, send one request, and read the reply
 *  until the server closes (HTTP/1.0 + "Connection: close"). No
 *  retransmission, one connection at a time, in-order segments only — but
 *  it really does talk to real servers out on the internet through QEMU's
 *  SLIRP NAT. This is the piece that turns "we have a NIC" into "we can
 *  fetch a web page".
 * ===================================================================== */

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static void put32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }

/* TCP/UDP checksum: internet checksum over a pseudo-header + the segment. */
static uint16_t l4_checksum(const uint8_t *sip, const uint8_t *dip,
                            uint8_t proto, const uint8_t *seg, int len) {
    uint32_t sum = 0;
    sum += sip[0]<<8|sip[1]; sum += sip[2]<<8|sip[3];
    sum += dip[0]<<8|dip[1]; sum += dip[2]<<8|dip[3];
    sum += proto;
    sum += len;
    for (int i = 0; i + 1 < len; i += 2) sum += seg[i]<<8|seg[i+1];
    if (len & 1) sum += seg[len-1]<<8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t g_ipid = 0x100;

/* Build + send one TCP segment to dip:dport from our sport. */
static void tcp_send_seg(const uint8_t *dmac, const uint8_t *dip,
                         uint16_t sport, uint16_t dport,
                         uint32_t seq, uint32_t ack, uint8_t flags,
                         const uint8_t *data, int dlen) {
    uint8_t pkt[1600];
    const uint8_t *me = nic_mac();
    memcpy(pkt + 0, dmac, 6); memcpy(pkt + 6, me, 6); put16(pkt + 12, 0x0800);
    uint8_t *ip = pkt + 14, *tcp = pkt + 34;

    put16(tcp + 0, sport); put16(tcp + 2, dport);
    put32(tcp + 4, seq);   put32(tcp + 8, ack);
    tcp[12] = 5 << 4;                      /* data offset = 5 words, no options */
    tcp[13] = flags;
    put16(tcp + 14, 32768);                /* window */
    put16(tcp + 16, 0);                    /* checksum (fill below) */
    put16(tcp + 18, 0);                    /* urgent ptr */
    if (dlen > 0) memcpy(tcp + 20, data, dlen);
    int tcplen = 20 + dlen;
    put16(tcp + 16, l4_checksum(OUR_IP, dip, 6, tcp, tcplen));

    ip[0] = 0x45; ip[1] = 0; put16(ip + 2, 20 + tcplen);
    put16(ip + 4, g_ipid++); put16(ip + 6, 0x4000 /*DF*/); ip[8] = 64; ip[9] = 6;
    put16(ip + 10, 0); memcpy(ip + 12, OUR_IP, 4); memcpy(ip + 16, dip, 4);
    put16(ip + 10, inet_checksum(ip, 20));

    nic_send(pkt, 34 + tcplen);
}

/* Receive the next TCP segment for our connection (server dip:80 -> us:sport).
 * Returns the IP-payload TCP header pointer via *tcp_out and the data length;
 * 0 on timeout. */
static int tcp_recv_seg(uint8_t *buf, int max, const uint8_t *dip,
                        uint16_t sport, uint16_t dport, uint64_t ticks,
                        uint8_t **tcp_out, int *dlen_out) {
    uint64_t deadline = timer_ticks() + ticks;
    while (timer_ticks() < deadline) {
        int len = nic_receive(buf, max);
        if (len < 34) continue;
        if (get16(buf + 12) != 0x0800 || buf[14 + 9] != 6) continue;   /* IPv4/TCP */
        if (memcmp(buf + 26, dip, 4) != 0) continue;                   /* from server */
        int ihl = (buf[14] & 0x0F) * 4;
        if (ihl < 20 || 14 + ihl + 20 > len) continue;     /* need a full TCP header */
        uint8_t *tcp = buf + 14 + ihl;
        if (get16(tcp + 0) != dport || get16(tcp + 2) != sport) continue;
        int thl = (tcp[12] >> 4) * 4;
        if (thl < 20 || 14 + ihl + thl > len) continue;    /* header must fit in frame */
        /* Trust only the bytes we actually received: the IP header's claimed
         * total length is server-controlled, so clamp it to `len` before using
         * it to size a memcpy (otherwise a runt/forged frame -> OOB read). */
        int iptotal = get16(buf + 16);
        if (iptotal > len - 14) iptotal = len - 14;
        int dlen = iptotal - ihl - thl;
        if (dlen < 0) dlen = 0;
        *tcp_out = tcp;
        *dlen_out = dlen;
        return 1;
    }
    return 0;
}

/* ---------------- reusable TCP stream (for HTTP and, later, TLS) ----------- */

static void tcp_rx_reset(void);   /* reset reassembly + FIN state (defined below) */

/* Open a connection to ip:port (routed via the gateway). 0 on success, -1 on
 * failure; fills the connection state. */
int tcp_connect(tcp_conn *c, const uint8_t ip[4], uint16_t port) {
    memset(c, 0, sizeof(*c));
    tcp_rx_reset();               /* fresh reassembly + FIN state for this conn */
    memcpy(c->ip, ip, 4);
    if (!arp_resolve(GW_IP, c->gw)) return -1;
    c->dport = port;
    static uint32_t conn_ctr = 0; conn_ctr++;   /* a monotonic per-connection nonce: the 100 Hz clock alone repeats across rapid reconnects (a browser's back-to-back sub-resource fetches), reusing port+ISN and risking a stale SYN-ACK/segment from the prior connection being accepted on the reused 4-tuple */
    c->sport = (uint16_t)(40000 + ((timer_ticks() + conn_ctr * 2179u) & 0x3FFF));
    c->myseq = ((uint32_t)(timer_ticks() * 2654435761u) ^ (conn_ctr * 0x9E3779B9u)) | 1;
    uint8_t buf[1600];
    for (int attempt = 0; attempt < 4; attempt++) {
        tcp_send_seg(c->gw, c->ip, c->sport, port, c->myseq, 0, TCP_SYN, 0, 0);
        uint8_t *tcp; int dlen;
        uint64_t deadline = timer_ticks() + 120;
        while (timer_ticks() < deadline) {
            if (!tcp_recv_seg(buf, sizeof(buf), c->ip, c->sport, port, 30, &tcp, &dlen)) continue;
            uint8_t fl = tcp[13];
            if (fl & TCP_RST) return -1;
            if ((fl & TCP_SYN) && (fl & TCP_ACK) && get32(tcp + 8) == c->myseq + 1) {
                c->theirseq = get32(tcp + 4) + 1;
                c->myseq += 1;
                tcp_send_seg(c->gw, c->ip, c->sport, port, c->myseq, c->theirseq, TCP_ACK, 0, 0);
                c->up = 1;
                return 0;
            }
        }
    }
    return -1;
}

/* Send `len` bytes on the connection. Returns len, or -1. */
int tcp_write(tcp_conn *c, const uint8_t *data, int len) {
    if (!c->up) return -1;
    int off = 0;
    while (off < len) {                                  /* segment to <=1400-byte chunks */
        int chunk = len - off; if (chunk > 1400) chunk = 1400;
        tcp_send_seg(c->gw, c->ip, c->sport, c->dport, c->myseq, c->theirseq,
                     TCP_PSH | TCP_ACK, data + off, chunk);
        c->myseq += chunk; off += chunk;
    }
    return len;
}

/* ---- out-of-order reassembly (one connection at a time, serialized) -------
 * A dropped or reordered segment leaves a gap in the byte stream. The naive
 * approach (drop everything after the gap and re-ACK) forces the whole tail to
 * be retransmitted and can stall a fast CDN burst, where one segment is lost to
 * an RX-ring overflow and the next dozen arrive ahead of the retransmit. So we
 * buffer those post-gap bytes here and deliver them the instant the gap fills.
 *
 * `ooo_base` is the sequence number of ooo_buf[0]; `ooo_have` marks which bytes
 * have arrived; `ooo_hi` is the highest stored offset+1. The store is single-
 * instance because TCP here is single-connection (same serialization that makes
 * the static crypto buffers safe). Reset per connection in tcp_connect(). */
#define OOO_CAP (96 * 1024)
static uint8_t  ooo_buf[OOO_CAP];
static uint8_t  ooo_have[OOO_CAP / 8];
static uint32_t ooo_base;
static int      ooo_active;          /* 1 while buffered future bytes exist */
static int      ooo_hi;              /* highest stored offset+1 (0 = empty)  */
static int      fin_seen;            /* peer FIN observed (maybe out of order) */
static uint32_t fin_at;              /* the FIN's sequence number (= data end) */

/* wraparound-safe 32-bit sequence comparisons */
static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline int ooo_bit(int i) { return ooo_have[i >> 3] & (1 << (i & 7)); }
static inline void ooo_setbit(int i) { ooo_have[i >> 3] |= (uint8_t)(1 << (i & 7)); }

/* Reset only the reassembly buffer. The peer FIN (fin_seen/fin_at) is NOT
 * cleared here: a FIN can be recorded while bytes are still buffered, and the
 * drain that empties the buffer calls this — clearing fin_seen there would lose
 * the FIN and the connection would never close cleanly. FIN state is per
 * connection and is reset in tcp_connect via tcp_rx_reset(). */
static void ooo_reset(void) { ooo_active = 0; ooo_hi = 0; }
static void tcp_rx_reset(void) { ooo_reset(); fin_seen = 0; }

/* Stash a future segment [seq, seq+dlen) (seq > theirseq) for later delivery. */
static void ooo_store(uint32_t theirseq, uint32_t seq, const uint8_t *data, int dlen) {
    if (!ooo_active) {                       /* anchor a fresh window at the gap */
        ooo_active = 1; ooo_base = theirseq; ooo_hi = 0;
        memset(ooo_have, 0, sizeof(ooo_have));
    }
    /* off is the serial distance seq-ooo_base. seq_gt() upstream only proves it is
     * serially positive, so it can be as large as ~2^31 for a far-future or forged
     * segment; test it WITHOUT the addition off+dlen, which would signed-overflow
     * (no -fwrapv) and wrap negative, slipping past the bound into an OOB write.
     * dlen is bounded by the 1600-byte rx frame, so OOO_CAP-dlen is a safe int. */
    int off = (int)(seq - ooo_base);         /* base <= theirseq < seq => off>0  */
    if (off < 0 || dlen < 0 || dlen > OOO_CAP || off > OOO_CAP - dlen) return;  /* outside window: drop */
    memcpy(ooo_buf + off, data, dlen);
    for (int i = off; i < off + dlen; i++) ooo_setbit(i);
    if (off + dlen > ooo_hi) ooo_hi = off + dlen;
}

/* Deliver any buffered bytes now contiguous with theirseq into out[]. */
static void ooo_drain(tcp_conn *c, uint8_t *out, int *total, int max) {
    if (!ooo_active) return;
    int rel = (int)(c->theirseq - ooo_base);
    while (rel >= 0 && rel < ooo_hi && ooo_bit(rel) && *total < max) {
        int run = rel; while (run < ooo_hi && ooo_bit(run)) run++;
        int runlen = run - rel;
        int n = runlen; if (*total + n > max) n = max - *total;
        memcpy(out + *total, ooo_buf + rel, n);
        *total += n; c->theirseq += n; rel += n;
        if (n < runlen) break;               /* out filled mid-run: stop */
    }
    if (rel >= ooo_hi) ooo_reset();          /* nothing more buffered ahead */
}

/* Read up to `max` bytes of in-order stream data (waits up to `ticks`). Returns
 * bytes read (0 on timeout), or -1 if the connection has closed/reset. ACKs,
 * buffers out-of-order segments, and tracks the peer FIN (returns -1 once
 * everything up to and including the FIN has been delivered). */
int tcp_read(tcp_conn *c, uint8_t *out, int max, uint64_t ticks) {
    if (!c->up) return -1;
    uint8_t buf[1600];
    int total = 0;
    uint64_t deadline = timer_ticks() + ticks;
    ooo_drain(c, out, &total, max);          /* flush data buffered last call */
    if (fin_seen && seq_le(fin_at, c->theirseq)) {   /* honor once theirseq REACHES OR PASSES the FIN (wrap-safe; `==` hung forever on any overshoot) */ /* all data up to the FIN delivered */
        c->theirseq = fin_at + 1;
        tcp_send_seg(c->gw, c->ip, c->sport, c->dport, c->myseq, c->theirseq, TCP_FIN | TCP_ACK, 0, 0);
        c->up = 0; return total > 0 ? total : -1;
    }
    while (timer_ticks() < deadline && total < max) {
        uint8_t *tcp; int dlen;
        if (!tcp_recv_seg(buf, sizeof(buf), c->ip, c->sport, c->dport, 20, &tcp, &dlen)) {
            if (total > 0) break;                        /* return what we have */
            continue;
        }
        uint8_t fl = tcp[13];
        uint32_t seq = get32(tcp + 4);
        if (fl & TCP_RST) { c->up = 0; return total > 0 ? total : -1; }
        int thl = (tcp[12] >> 4) * 4;
        uint8_t *data = tcp + thl;
        if (fl & TCP_FIN) {                        /* FIN occupies sequence seq+dlen */
            uint32_t f = seq + dlen;               /* never move fin_at BACKWARD: a stale or */
            if (!fin_seen || seq_gt(f, fin_at)) fin_at = f;   /* overlapping retransmitted FIN */
            fin_seen = 1;                          /* with a lower end must not strand the close */
        }

        if (dlen > 0 && seq_le(seq, c->theirseq) && seq_lt(c->theirseq, seq + dlen)) {
            /* segment contains the next expected byte (in-order, or an overlapping
             * retransmit) — deliver the part at/after theirseq, only what fits. */
            int skip = (int)(c->theirseq - seq);
            int n = dlen - skip; if (total + n > max) n = max - total;
            if (n > 0) { memcpy(out + total, data + skip, n); total += n; c->theirseq += n; }
            ooo_drain(c, out, &total, max);              /* gap may now be bridged */
            tcp_send_seg(c->gw, c->ip, c->sport, c->dport, c->myseq, c->theirseq, TCP_ACK, 0, 0);
        } else if (dlen > 0 && seq_gt(seq, c->theirseq)) {
            ooo_store(c->theirseq, seq, data, dlen);      /* future segment: buffer */
            tcp_send_seg(c->gw, c->ip, c->sport, c->dport, c->myseq, c->theirseq, TCP_ACK, 0, 0);
        } else {
            tcp_send_seg(c->gw, c->ip, c->sport, c->dport, c->myseq, c->theirseq, TCP_ACK, 0, 0);
        }

        /* Honour the FIN only once every byte up to it has been delivered. A FIN
         * that arrived out of order (gap still open) waits; the drain above + the
         * top-of-call check consume it as soon as theirseq reaches it. */
        if (fin_seen && seq_le(fin_at, c->theirseq)) {   /* honor once theirseq REACHES OR PASSES the FIN (wrap-safe; `==` hung forever on any overshoot) */
            c->theirseq = fin_at + 1;
            tcp_send_seg(c->gw, c->ip, c->sport, c->dport, c->myseq, c->theirseq, TCP_FIN | TCP_ACK, 0, 0);
            c->up = 0;
            return total > 0 ? total : -1;
        }
    }
    return total;
}

void tcp_close(tcp_conn *c) {
    fin_seen = 0; fin_at = 0;     /* per-connection teardown: scrub the global peer-FIN state so a latched-but-unhonored FIN can't prematurely close the NEXT connection on a reused 4-tuple (was cleared only at the next tcp_connect) */
    if (!c->up) return;
    tcp_send_seg(c->gw, c->ip, c->sport, c->dport, c->myseq, c->theirseq, TCP_FIN | TCP_ACK, 0, 0);
    c->up = 0;
}

/* HTTP/1.0 GET http://host/path -> writes the raw response (headers+body) into
 * out (up to max bytes). Returns bytes received, or -1 on error. */
int http_get(const char *host, const char *path, char *out, int max) {
    if (max <= 0) return 0;
    uint8_t ip[4];
    if (dns_resolve(host, ip) != 0) return -1;
    tcp_conn c;
    if (tcp_connect(&c, ip, 80) != 0) return -1;

    char req[512]; int rl = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\nUser-Agent: OS-DEV/0.1\r\n\r\n" };
    for (unsigned k = 0; k < sizeof(parts)/sizeof(parts[0]); k++)
        for (const char *s = parts[k]; *s && rl < (int)sizeof(req); s++) req[rl++] = *s;
    tcp_write(&c, (uint8_t *)req, rl);

    int total = 0;
    uint64_t budget = timer_ticks() + 600;
    while (c.up && total < max && timer_ticks() < budget) {
        int n = tcp_read(&c, (uint8_t *)out + total, max - total, 60);
        if (n < 0) break;
        total += n;
    }
    tcp_close(&c);
    return total;
}

/* ===================================================================== *
 *  /net/tcp — Plan 9 style "sockets as files" (M1110). No fd table: the
 *  VFS routes /net/tcp/<...> here. Read /net/tcp/clone -> a connection slot
 *  index; write "connect host!port" to /net/tcp/<n>/ctl; then read/write
 *  /net/tcp/<n>/data. Wraps the existing tcp_connect/tcp_write/tcp_read.
 * ===================================================================== */
#define NETCONN_N 4
static struct { int used, connected; tcp_conn c; } nconn[NETCONN_N];

static int nfs_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
/* parse a leading "<digits>/" off *sub, returning the index and advancing *sub past it; -1 if malformed */
static int nfs_idx(const char **sub) {
    const char *s = *sub; int n = 0, any = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; any = 1; }
    if (!any || *s != '/') return -1;
    *sub = s + 1;
    return n;
}

long netfs_read(const char *sub, void *buf, unsigned long max) {
    if (nfs_streq(sub, "clone")) {                       /* allocate a connection slot */
        for (int i = 0; i < NETCONN_N; i++) if (!nconn[i].used) {
            nconn[i].used = 1; nconn[i].connected = 0;
            char *b = (char *)buf; int p = 0;
            if (i >= 10 && p < (int)max) b[p++] = (char)('0' + i / 10);
            if (p < (int)max) b[p++] = (char)('0' + i % 10);
            if (p < (int)max) b[p++] = '\n';
            return p;
        }
        return -1;                                       /* all slots in use */
    }
    int idx = nfs_idx(&sub);
    if (idx < 0 || idx >= NETCONN_N || !nconn[idx].used) return -1;
    if (nfs_streq(sub, "data")) {
        if (!nconn[idx].connected) return -1;
        __asm__ volatile("sti");                         /* TCP read needs the timer for its timeout */
        int n = tcp_read(&nconn[idx].c, buf, (int)max, 200);   /* up to ~2 s for data */
        return n < 0 ? 0 : n;                            /* closed -> EOF (0) */
    }
    return -1;
}

long netfs_write(const char *sub, const void *buf, unsigned long len) {
    int idx = nfs_idx(&sub);
    if (idx < 0 || idx >= NETCONN_N || !nconn[idx].used) return -1;

    if (nfs_streq(sub, "ctl")) {
        char cmd[128]; unsigned long n = len < 127 ? len : 127;
        for (unsigned long i = 0; i < n; i++) cmd[i] = ((const char *)buf)[i];
        cmd[n] = 0;
        while (n && (cmd[n-1] == '\n' || cmd[n-1] == '\r' || cmd[n-1] == ' ')) cmd[--n] = 0;
        if (cmd[0]=='c'&&cmd[1]=='l'&&cmd[2]=='o') {     /* "close" */
            if (nconn[idx].connected) { __asm__ volatile("sti"); tcp_close(&nconn[idx].c); }
            nconn[idx].used = 0; nconn[idx].connected = 0;
            return (long)len;
        }
        if (cmd[0]=='c'&&cmd[1]=='o'&&cmd[2]=='n') {     /* "connect host!port" */
            char *h = cmd; while (*h && *h != ' ') h++; while (*h == ' ') h++;   /* skip "connect " */
            char *bang = h; while (*bang && *bang != '!' && *bang != ':') bang++;   /* host!port or host:port */
            if (*bang != '!' && *bang != ':') return -1;
            *bang = 0;
            uint16_t port = 0; for (char *p = bang + 1; *p >= '0' && *p <= '9'; p++) port = (uint16_t)(port * 10 + (*p - '0'));
            if (!port) return -1;
            uint8_t ip[4];
            if (parse_ipv4(h, ip) != 0) {                /* a hostname -> resolve it */
                __asm__ volatile("sti");
                if (dns_resolve(h, ip) != 0) return -1;
            }
            __asm__ volatile("sti");                     /* the handshake needs the timer */
            if (tcp_connect(&nconn[idx].c, ip, port) != 0) return -1;
            nconn[idx].connected = 1;
            return (long)len;
        }
        return -1;
    }
    if (nfs_streq(sub, "data")) {
        if (!nconn[idx].connected) return -1;
        __asm__ volatile("sti");
        int w = tcp_write(&nconn[idx].c, (const uint8_t *)buf, (int)len);
        return w < 0 ? -1 : w;
    }
    return -1;
}

/* HTTP POST over plain TCP (M702). Sends the headers (with Content-Type + Content-Length)
 * then the body as a second write — so an arbitrary body size isn't bounded by the small
 * request buffer. Returns the raw response length (headers+body), or -1 on failure. */
int http_post(const char *host, const char *path, const char *ctype,
              const char *body, int bodylen, char *out, int max) {
    if (max <= 0) return 0;
    if (bodylen < 0) bodylen = 0;
    uint8_t ip[4];
    if (dns_resolve(host, ip) != 0) return -1;
    tcp_conn c;
    if (tcp_connect(&c, ip, 80) != 0) return -1;

    char clen[12]; { unsigned b = (unsigned)bodylen; int t = 0; char tmp[12];
        do { tmp[t++] = (char)('0' + b % 10); b /= 10; } while (b && t < 11);
        int ci = 0; while (t) clen[ci++] = tmp[--t]; clen[ci] = 0; }
    char req[640]; int rl = 0;
    const char *parts[] = { "POST ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nContent-Type: ", ctype ? ctype : "text/plain",
                            "\r\nContent-Length: ", clen,
                            "\r\nConnection: close\r\nUser-Agent: OS-DEV/0.1\r\n\r\n" };
    for (unsigned k = 0; k < sizeof(parts)/sizeof(parts[0]); k++)
        for (const char *s = parts[k]; *s && rl < (int)sizeof(req); s++) req[rl++] = *s;
    tcp_write(&c, (uint8_t *)req, rl);
    if (bodylen > 0) tcp_write(&c, (uint8_t *)body, bodylen);

    int total = 0;
    uint64_t budget = timer_ticks() + 600;
    while (c.up && total < max && timer_ticks() < budget) {
        int n = tcp_read(&c, (uint8_t *)out + total, max - total, 60);
        if (n < 0) break;
        total += n;
    }
    tcp_close(&c);
    return total;
}

void net_demo(void) {
    if (nic_init() != 0) {
        kprintf("[net] no supported NIC found (tried e1000, rtl8139).\n\n");
        return;
    }

    kprintf("[net] %s up. our MAC = ", nic_name());
    print_mac(nic_mac());
    kprintf(", IP = 10.0.2.15\n");

    uint8_t gw_mac[6];
    if (!arp_resolve(GW_IP, gw_mac)) {
        kprintf("[net] ARP for 10.0.2.2 timed out.\n\n");
        return;
    }
    kprintf("[net] ARP: 10.0.2.2 is at ");
    print_mac(gw_mac);
    kprintf("\n");

    int got = 0;
    for (uint16_t seq = 1; seq <= 3; seq++) {
        if (ping(GW_IP, gw_mac, seq)) {
            kprintf("[net] ping 10.0.2.2: reply seq=%u\n", seq);
            got++;
        } else {
            kprintf("[net] ping 10.0.2.2: seq=%u timed out\n", seq);
        }
    }
    kprintf("[net] %d/3 echo replies. Networking works!\n", got);

    /* Prove the TCP/HTTP stack: fetch a real page over the internet. */
    static char page[2048];
    int n = http_get("example.com", "/", page, sizeof(page) - 1);
    if (n > 0) {
        page[n] = 0;
        int eol = 0; while (eol < n && page[eol] != '\r' && page[eol] != '\n') eol++;
        page[eol] = 0;
        kprintf("[net] HTTP GET example.com -> %d bytes, status: %s\n\n", n, page);
    } else {
        kprintf("[net] HTTP GET example.com failed (no internet route?)\n\n");
    }

    /* Prove the from-scratch TLS 1.3 stack end to end: a real HTTPS GET exercises
     * the handshake (X25519 key share), the AEAD record layer (AES-GCM/ChaCha20),
     * X.509 chain parsing + path validation to a trusted root, hostname matching,
     * and the CertificateVerify signature check (ECDSA/RSA over our own bignum).
     * Non-fatal: an offline host or a blocked :443 just reports failure. */
    static uint8_t tpage[4096];
    int tn = tls_get("example.com", "/", tpage, sizeof(tpage) - 1, (uint32_t)timer_ticks());
    if (tn > 0) {
        tpage[tn] = 0;
        int eol = 0; while (eol < tn && tpage[eol] != '\r' && tpage[eol] != '\n') eol++;
        tpage[eol] = 0;
        kprintf("[tls] HTTPS GET example.com -> %d bytes, status: %s\n", tn, (char *)tpage);
        kprintf("[tls] cert CN=%s expires %s | chain=%s host=%s certverify=%s\n\n",
                tls_leaf_cn(), tls_leaf_expiry(),
                tls_chain_anchored()      ? "trusted-root" : "UNVERIFIED",
                tls_host_match() == 1 ? "match" : tls_host_match() == 0 ? "MISMATCH" : "n/a",
                tls_cert_status() == 0  ? "ok"   : "FAIL");
    } else {
        kprintf("[tls] HTTPS GET example.com failed (no internet route, blocked :443, or handshake error)\n\n");
    }
}

/* --- /proc/net (M1080): the network state nothing else exposed -------------
 * Interface (IP/MAC/gateway/DNS) plus the live ARP and DNS caches with their
 * freshness — the kernel-side counterpart to Linux's /proc/net/{arp,…}. Pure
 * read-only formatting over state already kept here; procfs.c routes it. */
static int np_str(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int np_dec(char *b, int p, int max, uint64_t v) {
    char t[20]; int n = 0; if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n]; return p;
}
static int np_ip(char *b, int p, int max, const uint8_t *ip) {
    for (int i = 0; i < 4; i++) { if (i && p < max - 1) b[p++] = '.'; p = np_dec(b, p, max, ip[i]); }
    return p;
}
static int np_mac(char *b, int p, int max, const uint8_t *m) {
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i && p < max - 1) b[p++] = ':';
        if (p < max - 1) b[p++] = hex[m[i] >> 4];
        if (p < max - 1) b[p++] = hex[m[i] & 15];
    }
    return p;
}

int net_proc(char *b, int max) {
    if (!b || max < 2) return 0;
    uint64_t now = timer_ticks();
    int p = 0;
    p = np_str(b, p, max, "iface    ip ");   p = np_ip(b, p, max, net_ip());
    p = np_str(b, p, max, "  mac ");          p = np_mac(b, p, max, net_mac());
    p = np_str(b, p, max, "\ngateway  ");     p = np_ip(b, p, max, net_gateway());
    p = np_str(b, p, max, "    dns ");        p = np_ip(b, p, max, net_dns());
    p = np_str(b, p, max, "\nroute    default via ");  p = np_ip(b, p, max, net_gateway());
    p = np_str(b, p, max, "\n\nARP cache (IP -> MAC):\n");
    int any = 0;
    for (int i = 0; i < ARP_CACHE_N; i++) if (arp_cache[i].used) {
        p = np_str(b, p, max, "  ");  p = np_ip(b, p, max, arp_cache[i].ip);
        p = np_str(b, p, max, "  ");  p = np_mac(b, p, max, arp_cache[i].mac);
        p = np_str(b, p, max, now < arp_cache[i].exp ? "  fresh\n" : "  stale\n");
        any = 1;
    }
    if (!any) p = np_str(b, p, max, "  (empty)\n");
    p = np_str(b, p, max, "\nDNS cache (host -> IP):\n");
    any = 0;
    for (int i = 0; i < DNS_CACHE_N; i++) if (dns_cache[i].used) {
        p = np_str(b, p, max, "  ");   p = np_str(b, p, max, dns_cache[i].host);
        p = np_str(b, p, max, " -> "); p = np_ip(b, p, max, dns_cache[i].ip);
        p = np_str(b, p, max, now < dns_cache[i].exp ? "  fresh\n" : "  stale\n");
        any = 1;
    }
    if (!any) p = np_str(b, p, max, "  (empty)\n");
    if (p < max) b[p] = 0;
    return p;
}
