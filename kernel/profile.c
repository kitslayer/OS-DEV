/*
 * profile.c — a statistical sampling profiler over the kallsyms table (M1086).
 *
 * `prof_tick` is called from the timer IRQ with the interrupted RIP/CS; when
 * profiling is on and the sample is kernel-mode, it finds the enclosing
 * function (kallsyms) and bumps that function's sample count. So running a
 * kernel-heavy operation while profiling (e.g. a long `js -e` loop, which runs
 * in the in-kernel JS engine) makes the hot kernel functions float to the top
 * of `/proc/profile`.
 *
 * Ring-3 samples can't be symbolized here (app code isn't in the kernel symbol
 * table) but are still counted, aggregated by PROCESS NAME (M1510) — otherwise
 * an increasing share of the system (jsrun/imgdec/httpget/webview, the parsers
 * moved out of ring 0 for security) is invisible to this profiler entirely,
 * which would make "where does the time go" measurements on the current OS
 * systematically misleading (a workload that's genuinely CPU-heavy in ring 3
 * would misreport as mostly idle). Not per-function — that would need each
 * ELF's own symbol table — but "which program" is real, useful signal on its
 * own: it tells you where to point a *host-side* profiler/timer next.
 */
#include "profile.h"
#include "ksyms.h"
#include "task.h"
#include "app.h"

#define PROF_N 64                       /* distinct kernel functions tracked */
#define PROF_PN 16                      /* distinct ring-3 process names tracked */
static int      prof_on;
static uint32_t prof_total;             /* total kernel-mode samples taken */
static uint32_t prof_ring3_total;       /* total ring-3 samples taken */
static struct { unsigned long base; uint32_t count; } pb[PROF_N];
static int      pn;
static struct { char name[24]; uint32_t count; } pp[PROF_PN];
static int      ppn;

static int peq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

void prof_tick(uint64_t rip, uint64_t cs) {
    if (!prof_on) return;
    if ((cs & 3) != 0) {                            /* ring-3: tally by process name, not by address */
        task_t *t = task_self();
        const char *name = (t && t->proc) ? app_title((app_t *)t->proc) : 0;
        if (!name || !name[0]) name = "?";
        prof_ring3_total++;
        for (int i = 0; i < ppn; i++) if (peq(pp[i].name, name)) { pp[i].count++; return; }
        if (ppn < PROF_PN) {
            int j = 0; while (name[j] && j < (int)sizeof(pp[0].name) - 1) { pp[ppn].name[j] = name[j]; j++; }
            pp[ppn].name[j] = 0; pp[ppn].count = 1; ppn++;
        }
        return;
    }
    unsigned long off;
    const char *name = ksym_lookup(rip, &off);
    unsigned long base = name ? (rip - off) : rip;  /* aggregate by the function's start address */
    prof_total++;
    for (int i = 0; i < pn; i++) if (pb[i].base == base) { pb[i].count++; return; }
    if (pn < PROF_N) { pb[pn].base = base; pb[pn].count = 1; pn++; }   /* else drop: the hot ones are already tracked */
}

void prof_control(const char *cmd) {
    if (peq(cmd, "on"))         { pn = 0; prof_total = 0; ppn = 0; prof_ring3_total = 0; prof_on = 1; }   /* reset + start */
    else if (peq(cmd, "off"))   { prof_on = 0; }
    else if (peq(cmd, "reset")) { pn = 0; prof_total = 0; ppn = 0; prof_ring3_total = 0; }
}

static int sapp(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int sdec(char *b, int p, int max, unsigned long v) {
    char t[24]; int n = 0; if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n]; return p;
}

int prof_format(char *b, int max) {
    int p = 0;
    uint32_t grand = prof_total + prof_ring3_total;
    uint32_t tot = grand ? grand : 1;                 /* percentages are of the GRAND total (kernel + ring-3) */
    p = sapp(b, p, max, "profiling: ");  p = sapp(b, p, max, prof_on ? "ON" : "off");
    p = sapp(b, p, max, "  samples ");   p = sdec(b, p, max, grand);
    p = sapp(b, p, max, " (kernel ");    p = sdec(b, p, max, prof_total);
    p = sapp(b, p, max, ", ring-3 ");    p = sdec(b, p, max, prof_ring3_total); p = sapp(b, p, max, ")");
    p = sapp(b, p, max, "\n  SAMPLES   CPU%  KERNEL FUNCTION\n");
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
    if (shown == 0) p = sapp(b, p, max, "  (no kernel samples)\n");
    /* ring-3, by process name (not symbolized — a different program's own
     * symbol table would be needed for that); still real, useful signal on
     * which program to point a host-side profiler at next. */
    p = sapp(b, p, max, "\n  SAMPLES   CPU%  RING-3 PROCESS\n");
    int pshown = 0;
    char pused[PROF_PN] = {0};
    for (int rank = 0; rank < ppn && rank < PROF_PN && p < max - 40; rank++) {
        int best = -1;
        for (int i = 0; i < ppn; i++) if (!pused[i] && (best < 0 || pp[i].count > pp[best].count)) best = i;
        if (best < 0 || pp[best].count == 0) break;
        pused[best] = 1; pshown++;
        p = sapp(b, p, max, "  ");   p = sdec(b, p, max, pp[best].count);
        p = sapp(b, p, max, "    ");  p = sdec(b, p, max, (pp[best].count * 100ul) / tot); p = sapp(b, p, max, "%");
        p = sapp(b, p, max, "   ");  p = sapp(b, p, max, pp[best].name);
        p = sapp(b, p, max, "\n");
    }
    if (pshown == 0) p = sapp(b, p, max, "  (no ring-3 samples)\n");
    if (shown == 0 && pshown == 0) p = sapp(b, p, max, "  (`echo on > /proc/profile`, run something, then `echo off`)\n");
    if (p < max) b[p] = 0;
    return p;
}
