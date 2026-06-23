/*
 * profile.c — a statistical sampling profiler over the kallsyms table (M1086).
 *
 * `prof_tick` is called from the timer IRQ with the interrupted RIP/CS; when
 * profiling is on and the sample is kernel-mode, it finds the enclosing
 * function (kallsyms) and bumps that function's sample count. So running a
 * kernel-heavy operation while profiling (e.g. a long `js -e` loop, which runs
 * in the in-kernel JS engine) makes the hot kernel functions float to the top
 * of `/proc/profile`. Ring-3 samples are skipped — app code isn't in the kernel
 * symbol table, so it can't be symbolized here.
 */
#include "profile.h"
#include "ksyms.h"

#define PROF_N 64                       /* distinct functions tracked */
static int      prof_on;
static uint32_t prof_total;             /* total kernel-mode samples taken */
static struct { unsigned long base; uint32_t count; } pb[PROF_N];
static int      pn;

void prof_tick(uint64_t rip, uint64_t cs) {
    if (!prof_on || (cs & 3) != 0) return;          /* only kernel-mode samples are symbolizable */
    unsigned long off;
    const char *name = ksym_lookup(rip, &off);
    unsigned long base = name ? (rip - off) : rip;  /* aggregate by the function's start address */
    prof_total++;
    for (int i = 0; i < pn; i++) if (pb[i].base == base) { pb[i].count++; return; }
    if (pn < PROF_N) { pb[pn].base = base; pb[pn].count = 1; pn++; }   /* else drop: the hot ones are already tracked */
}

static int peq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

void prof_control(const char *cmd) {
    if (peq(cmd, "on"))         { pn = 0; prof_total = 0; prof_on = 1; }   /* reset + start */
    else if (peq(cmd, "off"))   { prof_on = 0; }
    else if (peq(cmd, "reset")) { pn = 0; prof_total = 0; }
}

static int sapp(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int sdec(char *b, int p, int max, unsigned long v) {
    char t[24]; int n = 0; if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n]; return p;
}

int prof_format(char *b, int max) {
    int p = 0;
    p = sapp(b, p, max, "profiling: ");  p = sapp(b, p, max, prof_on ? "ON" : "off");
    p = sapp(b, p, max, "  samples ");   p = sdec(b, p, max, prof_total);
    p = sapp(b, p, max, "\n  SAMPLES   CPU%  FUNCTION\n");
    uint32_t tot = prof_total ? prof_total : 1;
    /* selection-sort the top entries by count (pn is tiny) */
    int shown = 0;
    char used[PROF_N] = {0};
    for (int rank = 0; rank < pn && rank < 24 && p < max - 40; rank++) {
        int best = -1;
        for (int i = 0; i < pn; i++) if (!used[i] && (best < 0 || pb[i].count > pb[best].count)) best = i;
        if (best < 0 || pb[best].count == 0) break;
        used[best] = 1; shown++;
        unsigned long off; const char *name = ksym_lookup(pb[best].base, &off);
        p = sapp(b, p, max, "  ");   p = sdec(b, p, max, pb[best].count);
        p = sapp(b, p, max, "    ");  p = sdec(b, p, max, (pb[best].count * 100ul) / tot); p = sapp(b, p, max, "%");
        p = sapp(b, p, max, "   ");  p = sapp(b, p, max, name ? name : "?");
        p = sapp(b, p, max, "\n");
    }
    if (shown == 0) p = sapp(b, p, max, "  (no samples — `echo on > /proc/profile`, run a kernel-heavy op, then `echo off`)\n");
    if (p < max) b[p] = 0;
    return p;
}
