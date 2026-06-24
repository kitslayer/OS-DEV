/*
 * bpf.h — a tiny, verified in-kernel bytecode VM (eBPF-lite, M1127).
 *
 * Userspace uploads a small bytecode program (write it to /bpf) that the kernel
 * runs on each packet at the firewall hook (kernel/fw.c) to decide pass/drop —
 * programmable packet filtering, exactly what BPF was born for. The program is a
 * straight line of fixed-size instructions with FORWARD-ONLY conditional skips,
 * so it provably terminates (the PC only ever increases); a verifier (bpf_load)
 * rejects bad opcodes, register indices, context fields, and out-of-range skips,
 * and there is no memory access — only registers and the read-only context — so
 * a malicious program can't escape. The shape mirrors classic BPF without the
 * eBPF complexity.
 */
#pragma once
#include <stdint.h>

#define BPF_MAXINSN 64

/* opcodes */
enum {
    BPF_LDI = 1,   /* reg[a] = imm                                    */
    BPF_LDCTX,     /* reg[a] = ctx field `imm` (0..4, see bpf_ctx)    */
    BPF_ADD,       /* reg[a] += reg[b]                                */
    BPF_SUB,       /* reg[a] -= reg[b]                                */
    BPF_AND,       /* reg[a] &= reg[b]                                */
    BPF_OR,        /* reg[a] |= reg[b]                                */
    BPF_JEQ,       /* if reg[a] == imm: skip the next `b` instrs      */
    BPF_JNE,       /* if reg[a] != imm: skip the next `b` instrs      */
    BPF_RET,       /* return reg[a] (0 = DROP, nonzero = PASS)        */
    BPF_MAPINC,    /* bpf_map[reg[a] & (BPF_MAP_N-1)]++ : histogram aggregation (M1202) */
    BPF_OP_MAX
};

#define BPF_MAP_N 256   /* histogram cells a tracepoint program can increment (M1202) */

struct bpf_insn { uint8_t op, a, b, _pad; int32_t imm; };   /* 8 bytes */

/* The read-only context the firewall hook passes to the program. */
struct bpf_ctx { uint32_t dir, proto, sport, dport, len; }; /* fields 0..4 */

long bpf_load(const void *prog, unsigned long bytes);  /* verify + install (0 bytes = clear); 0/-1 */
int  bpf_loaded(void);                                 /* is a program installed? */
long bpf_run(const struct bpf_ctx *ctx);               /* run the global firewall program; verdict (0 drop / nonzero pass); 1 if none */
long bpf_run_prog(const struct bpf_insn *prog, int n, const struct bpf_ctx *ctx);  /* run an arbitrary program (seccomp, M1190) */
int  bpf_verify(const struct bpf_insn *in, int n);     /* verify a program (forward skips, has RET, in-range); 0 ok / -1 (M1190) */
int  bpf_format(char *out, int max);                   /* /proc/bpf: program + run/drop counters */

/* eBPF syscall tracepoint (M1202): a global program run on every syscall enter,
 * which counts by number into bpf_map via BPF_MAPINC (dtrace/bpftrace-style). */
long bpf_trace_load(const void *prog, unsigned long bytes); /* verify+install the trace program (0 bytes = clear + zero the map); 0/-1 */
int  bpf_trace_loaded(void);                               /* is a trace program installed? */
long bpf_trace_run(const struct bpf_ctx *ctx);             /* run it on one syscall enter */
uint64_t bpf_map_get(unsigned idx);                        /* read a histogram cell (idx & (BPF_MAP_N-1)) */
