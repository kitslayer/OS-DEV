/*
 * bpf.c — the eBPF-lite bytecode VM. See bpf.h.
 *
 * Verify-on-load + a tiny interpreter with forward-only control flow, so a
 * userspace-supplied program can run in the kernel's packet path safely: it
 * provably terminates (the PC strictly increases), touches no memory (only 8
 * registers + the read-only context), and is bounded in length.
 */
#include "bpf.h"

static struct bpf_insn g_prog[BPF_MAXINSN];
static int      g_n;                  /* installed instruction count (0 = none) */
static uint64_t g_runs, g_drops;      /* stats for /proc/bpf */
static uint64_t g_map[BPF_MAP_N];     /* histogram cells the BPF_MAPINC opcode writes (M1202) */

int bpf_loaded(void) { return g_n > 0; }

/* Verify a candidate program. Returns 0 if safe to run, -1 otherwise. */
int bpf_verify(const struct bpf_insn *in, int n) {
    if (n < 1 || n > BPF_MAXINSN) return -1;
    int has_ret = 0;
    for (int i = 0; i < n; i++) {
        const struct bpf_insn *x = &in[i];
        if (x->op < BPF_LDI || x->op >= BPF_OP_MAX) return -1;
        switch (x->op) {
            case BPF_LDI:                          if (x->a >= 8) return -1; break;
            case BPF_LDCTX:   if (x->imm < 0 || x->imm > 4 || x->a >= 8) return -1; break;
            case BPF_ADD: case BPF_SUB:
            case BPF_AND: case BPF_OR:             if (x->a >= 8 || x->b >= 8) return -1; break;
            case BPF_JEQ: case BPF_JNE:            /* skip is forward + must stay in range */
                if (x->a >= 8 || i + 1 + x->b > n) return -1; break;
            case BPF_RET:                          if (x->a >= 8) return -1; has_ret = 1; break;
            case BPF_MAPINC:                       if (x->a >= 8) return -1; break;
            default: return -1;
        }
    }
    if (!has_ret) return -1;                       /* must be able to return a verdict */
    return 0;
}

long bpf_load(const void *prog, unsigned long bytes) {
    if (bytes == 0) { g_n = 0; g_runs = g_drops = 0; return 0; }   /* clear */
    if (bytes % sizeof(struct bpf_insn) != 0) return -1;
    int n = (int)(bytes / sizeof(struct bpf_insn));
    const struct bpf_insn *in = (const struct bpf_insn *)prog;
    if (bpf_verify(in, n) != 0) return -1;
    for (int i = 0; i < n; i++) g_prog[i] = in[i];
    g_n = n; g_runs = g_drops = 0;
    return 0;
}

/* Run an ARBITRARY verified program against ctx; verdict (0 = drop/deny, nonzero
 * = pass/allow). Used by the global firewall (bpf_run) and per-process seccomp
 * filters (M1190). Pure over (prog, ctx) — no global state. */
long bpf_run_prog(const struct bpf_insn *prog, int n, const struct bpf_ctx *ctx) {
    if (n <= 0) return 1;                          /* no program -> pass */
    int64_t reg[8] = { 0 };
    int pc = 0, steps = 0;
    while (pc < n && steps++ < BPF_MAXINSN * 2) {    /* the step cap is belt-and-braces (skips are forward) */
        const struct bpf_insn *x = &prog[pc];
        switch (x->op) {
            case BPF_LDI:   reg[x->a] = x->imm; pc++; break;
            case BPF_LDCTX: {
                uint32_t v = 0;
                switch (x->imm) { case 0: v = ctx->dir; break; case 1: v = ctx->proto; break;
                                  case 2: v = ctx->sport; break; case 3: v = ctx->dport; break;
                                  case 4: v = ctx->len; break; }
                reg[x->a] = v; pc++; break;
            }
            case BPF_ADD: reg[x->a] += reg[x->b]; pc++; break;
            case BPF_SUB: reg[x->a] -= reg[x->b]; pc++; break;
            case BPF_AND: reg[x->a] &= reg[x->b]; pc++; break;
            case BPF_OR:  reg[x->a] |= reg[x->b]; pc++; break;
            case BPF_JEQ: pc += (reg[x->a] == x->imm) ? (x->b + 1) : 1; break;
            case BPF_JNE: pc += (reg[x->a] != x->imm) ? (x->b + 1) : 1; break;
            case BPF_RET: return (long)reg[x->a];
            case BPF_MAPINC: g_map[reg[x->a] & (BPF_MAP_N - 1)]++; pc++; break;  /* aggregation (M1202) */
            default: return 1;
        }
    }
    return 1;                                       /* fell off the end -> pass */
}

/* eBPF syscall tracepoint (M1202): a global program the kernel runs on every
 * syscall enter (kernel/syscall.c), which counts by number into g_map via the
 * BPF_MAPINC opcode — a verified, in-kernel "count syscalls by N" probe, the
 * dtrace/bpftrace idiom on top of the VM we already trust. Separate from the
 * firewall's g_prog so the two don't interfere. */
static struct bpf_insn g_trace[BPF_MAXINSN];
static int      g_trace_n;
static uint64_t g_trace_runs;

int bpf_trace_loaded(void) { return g_trace_n > 0; }
uint64_t bpf_map_get(unsigned idx) { return g_map[idx & (BPF_MAP_N - 1)]; }

long bpf_trace_load(const void *prog, unsigned long bytes) {
    if (bytes == 0) {                               /* clear: stop tracing + zero the histogram */
        g_trace_n = 0; g_trace_runs = 0;
        for (int i = 0; i < BPF_MAP_N; i++) g_map[i] = 0;
        return 0;
    }
    if (bytes % sizeof(struct bpf_insn) != 0) return -1;
    int n = (int)(bytes / sizeof(struct bpf_insn));
    const struct bpf_insn *in = (const struct bpf_insn *)prog;
    if (bpf_verify(in, n) != 0) return -1;
    for (int i = 0; i < n; i++) g_trace[i] = in[i];
    g_trace_n = n; g_trace_runs = 0;
    for (int i = 0; i < BPF_MAP_N; i++) g_map[i] = 0;   /* fresh histogram per load */
    return 0;
}

long bpf_trace_run(const struct bpf_ctx *ctx) {
    if (g_trace_n == 0) return 1;
    g_trace_runs++;
    return bpf_run_prog(g_trace, g_trace_n, ctx);
}

/* The global firewall program (M1127), with its /proc/bpf run/drop stats. */
long bpf_run(const struct bpf_ctx *ctx) {
    if (g_n == 0) return 1;                        /* no program -> pass */
    g_runs++;
    long v = bpf_run_prog(g_prog, g_n, ctx);
    if (v == 0) g_drops++;
    return v;
}

static int sa(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int sd(char *b, int p, int max, uint64_t v) {
    char t[20]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n]; return p;
}
int bpf_format(char *b, int max) {
    int p = 0;
    p = sa(b, p, max, "program: ");
    if (g_n == 0) p = sa(b, p, max, "(none -- write bytecode to /bpf)\n");
    else { p = sd(b, p, max, (uint64_t)g_n); p = sa(b, p, max, " instructions\n"); }
    p = sa(b, p, max, "runs:    "); p = sd(b, p, max, g_runs);  p = sa(b, p, max, "\n");
    p = sa(b, p, max, "drops:   "); p = sd(b, p, max, g_drops); p = sa(b, p, max, "\n");
    /* the syscall tracepoint program + its histogram (M1202) */
    p = sa(b, p, max, "trace:   ");
    if (g_trace_n == 0) p = sa(b, p, max, "(none)\n");
    else {
        p = sd(b, p, max, (uint64_t)g_trace_n); p = sa(b, p, max, " instructions, ");
        p = sd(b, p, max, g_trace_runs); p = sa(b, p, max, " runs\n");
    }
    p = sa(b, p, max, "map:    ");
    for (int i = 0; i < BPF_MAP_N; i++)
        if (g_map[i]) { p = sa(b, p, max, " ["); p = sd(b, p, max, (uint64_t)i); p = sa(b, p, max, "]="); p = sd(b, p, max, g_map[i]); }
    p = sa(b, p, max, "\n");
    if (p < max) b[p] = 0;
    return p;
}
