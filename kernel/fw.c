/*
 * fw.c — packet filter. See fw.h. First-match-wins over a small rule table;
 * no match => allow (default policy). Hooked at nic_send / nic_receive.
 */
#include "fw.h"
#include <stdint.h>

#define FW_MAX 16
struct rule { int used, dir, proto; uint16_t port; int drop; uint64_t hits; };
static struct rule g_rules[FW_MAX];
static int g_n;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

int fw_check(int dir, const void *frame, int len) {
    if (g_n == 0) return 1;                          /* no rules -> fast allow */
    const uint8_t *f = (const uint8_t *)frame;
    if (len < 34 || be16(f + 12) != 0x0800) return 1; /* non-IPv4 (ARP, ...) always passes */
    int ihl = (f[14] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl) return 1;
    int proto = f[14 + 9];
    uint16_t sport = 0, dport = 0;
    if ((proto == 6 || proto == 17) && len >= 14 + ihl + 4) {
        const uint8_t *l4 = f + 14 + ihl;
        sport = be16(l4); dport = be16(l4 + 2);
    }
    uint16_t rport = (dir == FW_OUT) ? dport : sport;   /* match on the remote port */
    for (int i = 0; i < g_n; i++) {
        struct rule *r = &g_rules[i];
        if (!r->used) continue;
        if (r->dir != 2 && r->dir != dir) continue;     /* 2 = both directions */
        if (r->proto && r->proto != proto) continue;
        if (r->port && r->port != rport) continue;
        r->hits++;                                       /* first match wins */
        return r->drop ? 0 : 1;
    }
    return 1;                                            /* default: allow */
}

/* --- /proc/fw --------------------------------------------------------------- */
static int tok_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
/* copy the next space-delimited token from *p into buf (cap n), advance *p */
static int next_tok(const char **p, char *buf, int n) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    int k = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n' && k < n - 1) buf[k++] = *s++;
    buf[k] = 0; *p = s;
    return k;
}

void fw_control(const char *cmd, int len) {
    (void)len;
    char a[12], b[12], c[12], d[12];
    const char *p = cmd;
    if (!next_tok(&p, a, sizeof a)) return;
    if (tok_eq(a, "flush")) { g_n = 0; for (int i = 0; i < FW_MAX; i++) g_rules[i].used = 0; return; }

    int drop;
    if (tok_eq(a, "drop")) drop = 1;
    else if (tok_eq(a, "allow")) drop = 0;
    else return;

    next_tok(&p, b, sizeof b);          /* direction */
    int dir = tok_eq(b, "out") ? FW_OUT : tok_eq(b, "in") ? FW_IN : 2;

    next_tok(&p, c, sizeof c);          /* protocol */
    int proto = tok_eq(c, "icmp") ? 1 : tok_eq(c, "tcp") ? 6 : tok_eq(c, "udp") ? 17 : 0;

    uint16_t port = 0;
    if (next_tok(&p, d, sizeof d)) { for (int i = 0; d[i] >= '0' && d[i] <= '9'; i++) port = (uint16_t)(port * 10 + (d[i] - '0')); }

    if (g_n >= FW_MAX) return;
    struct rule *r = &g_rules[g_n++];
    r->used = 1; r->dir = dir; r->proto = proto; r->port = port; r->drop = drop; r->hits = 0;
}

static int f_put(char *o, int p, int max, const char *s) { while (*s && p + 1 < max) o[p++] = *s++; return p; }
static int f_num(char *o, int p, int max, uint64_t v) {
    char t[24]; int ti = 0;
    if (!v) t[ti++] = '0'; else while (v) { t[ti++] = (char)('0' + v % 10); v /= 10; }
    while (ti && p + 1 < max) o[p++] = t[--ti];
    return p;
}

int fw_format(char *out, int max) {
    static const char *dirs[3]  = { "in  ", "out ", "both" };
    int p = 0;
    p = f_put(out, p, max, "packet filter (first-match; default allow)\n");
    if (g_n == 0) p = f_put(out, p, max, "  (no rules)\n");
    for (int i = 0; i < g_n; i++) {
        struct rule *r = &g_rules[i];
        if (!r->used) continue;
        p = f_put(out, p, max, "  ");
        p = f_put(out, p, max, r->drop ? "DROP  " : "ALLOW ");
        p = f_put(out, p, max, dirs[r->dir <= 2 ? r->dir : 2]);
        p = f_put(out, p, max, " ");
        p = f_put(out, p, max, r->proto == 1 ? "icmp" : r->proto == 6 ? "tcp " : r->proto == 17 ? "udp " : "any ");
        if (r->port) { p = f_put(out, p, max, " port "); p = f_num(out, p, max, r->port); }
        p = f_put(out, p, max, "  hits="); p = f_num(out, p, max, r->hits);
        p = f_put(out, p, max, "\n");
    }
    if (p < max) out[p] = 0;
    return p;
}
