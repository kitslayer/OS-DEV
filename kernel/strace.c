/*
 * strace.c — the per-process syscall trace ring. See strace.h.
 *
 * A fixed global ring of the most recent traced syscalls; each record carries
 * the pid so /proc/<pid>/strace can filter. Single producer (syscall_dispatch,
 * which runs interrupts-off as an interrupt gate) and a copying reader, so no
 * locking is needed — a torn read at worst shows a half-overwritten oldest line.
 */
#include "strace.h"

#define STRACE_N 128
static struct { int pid; char line[96]; } sr[STRACE_N];
static unsigned sr_total;                 /* monotonic count of records ever written */

static int sa(char *b, int p, int max, const char *s) {
    while (*s && p < max - 1) b[p++] = *s++;
    return p;
}
static int shex(char *b, int p, int max, uint64_t v) {        /* "0x" + minimal-width hex */
    if (p < max - 1) b[p++] = '0';
    if (p < max - 1) b[p++] = 'x';
    if (!v) { if (p < max - 1) b[p++] = '0'; return p; }
    char t[16]; int n = 0;
    while (v) { unsigned d = v & 0xF; t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (n && p < max - 1) b[p++] = t[--n];
    return p;
}
static int sdec(char *b, int p, int max, int v) {
    char t[12]; int n = 0; if (v < 0) { if (p < max - 1) b[p++] = '-'; v = -v; }
    if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n];
    return p;
}

void strace_record(int pid, const char *name, uint64_t a, uint64_t b, uint64_t c, uint64_t ret) {
    char *L = sr[sr_total % STRACE_N].line; int max = (int)sizeof sr[0].line; int p = 0;
    sr[sr_total % STRACE_N].pid = pid;
    p = sa(L, p, max, name);     p = sa(L, p, max, "(");
    p = shex(L, p, max, a);      p = sa(L, p, max, ", ");
    p = shex(L, p, max, b);      p = sa(L, p, max, ", ");
    p = shex(L, p, max, c);      p = sa(L, p, max, ") = ");
    p = shex(L, p, max, ret);
    L[p < max ? p : max - 1] = 0;
    sr_total++;
}

int strace_format(int pid, char *buf, int max) {
    int p = 0, any = 0;
    unsigned start = sr_total > STRACE_N ? sr_total - STRACE_N : 0;
    for (unsigned i = start; i < sr_total; i++) {
        int rpid = sr[i % STRACE_N].pid;
        if (pid > 0 && rpid != pid) continue;
        p = sa(buf, p, max, "  ");
        if (pid <= 0) { p = sa(buf, p, max, "["); p = sdec(buf, p, max, rpid); p = sa(buf, p, max, "] "); }
        p = sa(buf, p, max, sr[i % STRACE_N].line);
        p = sa(buf, p, max, "\n");
        any = 1;
    }
    if (!any) p = sa(buf, p, max, "  (no traced syscalls — write 'trace' to /proc/<pid>/ctl first)\n");
    if (p < max) buf[p] = 0;
    return p;
}
