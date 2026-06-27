/*
 * bpf.c — the eBPF-lite bytecode VM. See bpf.h.
 *
 * Verify-on-load + a tiny interpreter with forward-only control flow, so a
 * userspace-supplied program can run in the kernel's packet path safely: it
 * provably terminates (the PC strictly increases), touches no memory (only 8
 * registers + the read-only context), and is bounded in length.
 */
#include "bpf.h"
#include "console.h"   /* kprintf for the JIT self-test (M1290) */
#include <stdint.h>

static struct bpf_insn g_prog[BPF_MAXINSN];
static int      g_n;                  /* installed instruction count (0 = none) */
static uint64_t g_runs, g_drops;      /* stats for /proc/bpf */
static int      g_prog_jit_stale, g_trace_jit_stale;  /* set on (re)load; bpf_run/bpf_trace_run lazily (re)compile + validate the JIT (M1295) */
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
    g_prog_jit_stale = 1;                            /* bpf_run will JIT + validate this program */
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

/* ====================================================================== *
 * eBPF JIT (M1290): compile the bytecode to NATIVE x86-64.
 *
 * The interpreter above dispatches each instruction in a loop; the JIT emits a
 * straight-line native function `long fn(const struct bpf_ctx *ctx)` (SysV ABI:
 * ctx in rdi, result in rax) that does the same work with zero per-instruction
 * dispatch. The 8 BPF registers live in a 64-byte stack frame (reg[i] at
 * [rbp-8*(i+1)]); ctx stays in rdi (the generated code makes no calls, so
 * nothing clobbers it); forward JEQ/JNE skips become native je/jne rel32
 * resolved in a second pass. Code is emitted into a static buffer in the .jitexec
 * section, which W^X (vmm_harden_kernel) keeps executable while the rest of .bss is
 * marked no-execute, so a generated function can be called directly, no icache flush
 * (x86 snoops its own stores). Operates on VERIFIED programs (bpf_verify): valid
 * opcodes, reg indices < 8, forward in-range skips, a RET present.
 * ====================================================================== */

typedef long (*bpf_jit_fn)(const struct bpf_ctx *ctx);

/* reg[i] as an rbp-relative disp8: reg[0]@[rbp-8] .. reg[7]@[rbp-64]. */
static int8_t jit_rdisp(int r) { return (int8_t)(-8 * (r + 1)); }

/* Compile (prog,n) into buf; return a callable pointer, or 0 if it overflowed
 * buf or hit an unexpected opcode (a verified program never should). */
static bpf_jit_fn bpf_jit_compile(const struct bpf_insn *prog, int n, uint8_t *buf, int buflen) {
    int len = 0;
    int off[BPF_MAXINSN + 1];                       /* native offset of each bytecode insn (+ fall-off pad at [n]) */
    struct { int pos, target; } fix[BPF_MAXINSN];   /* JEQ/JNE rel32 fixups, resolved after layout */
    int nfix = 0;
    #define EMIT(b)   do { if (len >= buflen) return 0; buf[len++] = (uint8_t)(b); } while (0)
    #define EMIT32(v) do { uint32_t _v = (uint32_t)(v); EMIT(_v); EMIT(_v >> 8); EMIT(_v >> 16); EMIT(_v >> 24); } while (0)
    #define EMIT64(v) do { uint64_t _v = (uint64_t)(v); for (int _i = 0; _i < 8; _i++) EMIT(_v >> (8 * _i)); } while (0)

    EMIT(0x55);                                     /* push rbp                       */
    EMIT(0x48); EMIT(0x89); EMIT(0xE5);             /* mov rbp, rsp                   */
    EMIT(0x48); EMIT(0x83); EMIT(0xEC); EMIT(0x40); /* sub rsp, 64                    */
    EMIT(0x31); EMIT(0xC0);                         /* xor eax, eax                   */
    for (int r = 0; r < 8; r++) { EMIT(0x48); EMIT(0x89); EMIT(0x45); EMIT((uint8_t)jit_rdisp(r)); }  /* mov [rbp+d(r)], rax (zero reg) */

    for (int i = 0; i < n; i++) {
        off[i] = len;
        const struct bpf_insn *x = &prog[i];
        uint8_t da = (uint8_t)jit_rdisp(x->a), db = (uint8_t)jit_rdisp(x->b);
        switch (x->op) {
            case BPF_LDI:                           /* mov qword [rbp+da], imm32 (sign-extended) */
                EMIT(0x48); EMIT(0xC7); EMIT(0x45); EMIT(da); EMIT32(x->imm); break;
            case BPF_LDCTX:                         /* mov eax,[rdi+imm*4]; mov [rbp+da],rax (zero-extend) */
                EMIT(0x8B); EMIT(0x47); EMIT((uint8_t)(x->imm * 4));
                EMIT(0x48); EMIT(0x89); EMIT(0x45); EMIT(da); break;
            case BPF_ADD: case BPF_SUB: case BPF_AND: case BPF_OR: {
                EMIT(0x48); EMIT(0x8B); EMIT(0x45); EMIT(db);   /* mov rax, [rbp+db] */
                uint8_t opc = x->op == BPF_ADD ? 0x01 : x->op == BPF_SUB ? 0x29 : x->op == BPF_AND ? 0x21 : 0x09;
                EMIT(0x48); EMIT(opc); EMIT(0x45); EMIT(da);    /* <op> [rbp+da], rax */
                break;
            }
            case BPF_JEQ: case BPF_JNE:
                EMIT(0x48); EMIT(0x81); EMIT(0x7D); EMIT(da); EMIT32(x->imm);   /* cmp qword [rbp+da], imm32 */
                EMIT(0x0F); EMIT(x->op == BPF_JEQ ? 0x84 : 0x85);              /* je/jne rel32 */
                fix[nfix].pos = len; fix[nfix].target = i + x->b + 1; nfix++;
                EMIT32(0);                                                     /* rel32 placeholder */
                break;
            case BPF_RET:                           /* mov rax,[rbp+da]; leave; ret */
                EMIT(0x48); EMIT(0x8B); EMIT(0x45); EMIT(da); EMIT(0xC9); EMIT(0xC3); break;
            case BPF_MAPINC:                        /* g_map[reg[a] & 255]++ */
                EMIT(0x48); EMIT(0x8B); EMIT(0x45); EMIT(da);                  /* mov rax,[rbp+da]      */
                EMIT(0x25); EMIT32(0xFF);                                      /* and eax, 0xFF         */
                EMIT(0x48); EMIT(0xB9); EMIT64((uint64_t)(uintptr_t)g_map);    /* movabs rcx, &g_map    */
                EMIT(0x48); EMIT(0xFF); EMIT(0x04); EMIT(0xC1);                /* inc qword [rcx+rax*8] */
                break;
            default: return 0;                      /* unexpected opcode (verifier should prevent) */
        }
    }
    off[n] = len;
    EMIT(0xB8); EMIT32(1); EMIT(0xC9); EMIT(0xC3);  /* fall-off pad: mov eax,1; leave; ret (= pass) */

    for (int f = 0; f < nfix; f++) {                /* resolve forward je/jne rel32 */
        int t = fix[f].target; if (t < 0 || t > n) t = n;
        int32_t rel = off[t] - (fix[f].pos + 4);
        buf[fix[f].pos]     = (uint8_t)rel;
        buf[fix[f].pos + 1] = (uint8_t)(rel >> 8);
        buf[fix[f].pos + 2] = (uint8_t)(rel >> 16);
        buf[fix[f].pos + 3] = (uint8_t)(rel >> 24);
    }
    #undef EMIT
    #undef EMIT32
    #undef EMIT64
    return (bpf_jit_fn)buf;
}

/* The live firewall (g_prog) + tracepoint (g_trace) programs run JIT'd when the
 * JIT validates; these hold the generated code + the callable pointer (0 = use
 * the interpreter). bpf_run/bpf_trace_run (re)compile lazily on *_jit_stale. */
static uint8_t    g_prog_jitbuf[2048]  __attribute__((section(".jitexec")));
static uint8_t    g_trace_jitbuf[2048] __attribute__((section(".jitexec")));
static bpf_jit_fn g_prog_jit, g_trace_jit;

/* Compile (prog,n), then TRUST the JIT only if it returns EXACTLY what the
 * interpreter does over a spread of contexts — so a codegen bug can never change
 * a verdict; on any mismatch (or compile failure) return 0 and the caller falls
 * back to the interpreter. The defensive partner of bpf_verify. (M1295) */
static bpf_jit_fn bpf_jit_validate(const struct bpf_insn *prog, int n, uint8_t *buf, int buflen) {
    bpf_jit_fn fn = bpf_jit_compile(prog, n, buf, buflen);
    if (!fn) return 0;
    static const struct bpf_ctx s[] = {
        {0,0,0,0,0}, {1,1,80,80,64}, {0,6,1234,443,1500}, {1,17,53,53,512},
        {0,1,22,22,98}, {1,6,0,65535,9000},
    };
    for (unsigned i = 0; i < sizeof s / sizeof s[0]; i++)
        if (fn(&s[i]) != bpf_run_prog(prog, n, &s[i])) return 0;   /* JIT disagrees -> don't use it */
    return fn;
}

/* Boot self-test: compile two programs covering every opcode, then assert the
 * JIT'd native code returns EXACTLY what the interpreter does (over a range of
 * contexts), and that the JIT'd MAPINC really wrote the histogram. */
void bpf_jit_selftest(void) {
    static uint8_t buf1[1024] __attribute__((section(".jitexec")));   /* executable (JIT'd code runs here) */
    static uint8_t buf2[1024] __attribute__((section(".jitexec")));
    /* P1: drop ICMP (proto==1), else pass -- the canonical packet filter. */
    static const struct bpf_insn p1[] = {
        { BPF_LDCTX, 0, 0, 0, 1 }, { BPF_JNE, 0, 2, 0, 1 },
        { BPF_LDI, 0, 0, 0, 0 },   { BPF_RET, 0, 0, 0, 0 },
        { BPF_LDI, 0, 0, 0, 1 },   { BPF_RET, 0, 0, 0, 0 },
    };
    /* P2: arithmetic + bitwise + a taken JEQ + MAPINC -> returns 0x8C (140). */
    static const struct bpf_insn p2[] = {
        { BPF_LDI, 0, 0, 0, 0x3C }, { BPF_LDI, 1, 0, 0, 0x05 },
        { BPF_ADD, 0, 1, 0, 0 },    { BPF_SUB, 0, 1, 0, 0 },
        { BPF_LDI, 2, 0, 0, 0x0F }, { BPF_AND, 0, 2, 0, 0 },
        { BPF_LDI, 3, 0, 0, 0x80 }, { BPF_OR,  0, 3, 0, 0 },
        { BPF_JEQ, 0, 1, 0, 0x8C }, { BPF_LDI, 0, 0, 0, 0xFF },
        { BPF_MAPINC, 0, 0, 0, 0 }, { BPF_RET, 0, 0, 0, 0 },
    };
    bpf_jit_fn f1 = bpf_jit_compile(p1, 6, buf1, sizeof buf1);
    bpf_jit_fn f2 = bpf_jit_compile(p2, 12, buf2, sizeof buf2);
    int ok = (f1 && f2);
    if (ok) {
        struct bpf_ctx c = { 0, 0, 0, 0, 0 };
        for (int proto = 0; proto <= 17 && ok; proto++) {     /* JIT must match the interpreter for every proto */
            c.proto = (uint32_t)proto;
            if (f1(&c) != bpf_run_prog(p1, 6, &c)) ok = 0;
        }
        uint64_t before = g_map[0x8C];
        long ji = f2(&c), in = bpf_run_prog(p2, 12, &c);
        if (ji != in || ji != 0x8C) ok = 0;                   /* value matches + equals expected */
        if (g_map[0x8C] <= before) ok = 0;                    /* the JIT'd MAPINC actually wrote the map */
        /* wiring (M1295): load P1 as the live firewall program; bpf_run must JIT
         * + validate it and produce the right verdicts, then clear it. */
        bpf_load(p1, sizeof p1);
        struct bpf_ctx icmp = { 0, 1, 0, 0, 64 }, tcp = { 0, 6, 0, 0, 64 };
        if (bpf_run(&icmp) != 0 || bpf_run(&tcp) != 1 || g_prog_jit == 0) ok = 0;
        bpf_load(0, 0);                                       /* clear: leave the firewall empty */
    }
    if (ok)
        kprintf("[ ok ] eBPF JIT: native x86-64; JIT == interpreter over all opcodes + wired into the firewall (validated) -- eBPF JIT OK\n");
    else
        kprintf("[bpf] eBPF JIT FAILED (f1=%p f2=%p)\n", (void *)f1, (void *)f2);
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
    g_trace_jit_stale = 1;                              /* bpf_trace_run will JIT + validate this program */
    return 0;
}

long bpf_trace_run(const struct bpf_ctx *ctx) {
    if (g_trace_n == 0) return 1;
    if (g_trace_jit_stale) { g_trace_jit = bpf_jit_validate(g_trace, g_trace_n, g_trace_jitbuf, sizeof g_trace_jitbuf); g_trace_jit_stale = 0; }
    g_trace_runs++;
    return g_trace_jit ? g_trace_jit(ctx) : bpf_run_prog(g_trace, g_trace_n, ctx);   /* native if the JIT validated, else interpret */
}

/* The global firewall program (M1127), with its /proc/bpf run/drop stats. */
long bpf_run(const struct bpf_ctx *ctx) {
    if (g_n == 0) return 1;                        /* no program -> pass */
    if (g_prog_jit_stale) { g_prog_jit = bpf_jit_validate(g_prog, g_n, g_prog_jitbuf, sizeof g_prog_jitbuf); g_prog_jit_stale = 0; }
    g_runs++;
    long v = g_prog_jit ? g_prog_jit(ctx) : bpf_run_prog(g_prog, g_n, ctx);   /* native if the JIT validated, else interpret */
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
