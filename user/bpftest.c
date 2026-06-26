// bpftest.c — eBPF-lite in-kernel packet filter (M1127). Encode a tiny bytecode
// program that DROPS ICMP (the canonical BPF use — BPF began as a packet
// filter), upload it to /bpf, and watch ping go from working to dropped, with
// the kernel running our bytecode on every packet. /proc/bpf shows the stats.
#include "ulib.h"
#include "bpf.h"

static void pnum(long v) {
    if (v < 0) { print("-"); v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v);
    char b[21]; int i = 0; while (n) b[i++] = t[--n]; b[i] = 0; print(b);
}

int main(void) {
    long before = sys_ping();                       // baseline: ICMP echo to the gateway
    sys_setcolor(4); print("bpftest:"); sys_setcolor(0); print(" ping replies BEFORE the filter: "); pnum(before); print("\n");

    /* program: drop ICMP (IP proto 1), pass everything else.
     *   0: r0 = ctx.proto          (field 1)
     *   1: if r0 != 1, skip 2      (-> instr 4, the PASS)
     *   2: r0 = 0                  (DROP)
     *   3: return r0
     *   4: r0 = 1                  (PASS)
     *   5: return r0                                                         */
    struct bpf_insn prog[] = {
        { BPF_LDCTX, 0, 0, 0, 1 },
        { BPF_JNE,   0, 2, 0, 1 },
        { BPF_LDI,   0, 0, 0, 0 },
        { BPF_RET,   0, 0, 0, 0 },
        { BPF_LDI,   0, 0, 0, 1 },
        { BPF_RET,   0, 0, 0, 0 },
    };
    if (sys_writefile("/bpf", prog, sizeof prog) < 0) { print("bpftest: load failed\n"); sys_sleep(3000); return 1; }
    sys_setcolor(9); print("bpftest: uploaded a 6-instruction BPF filter (drop ICMP)\n"); sys_setcolor(0);

    long after = sys_ping();                         // now our bytecode drops the echo requests
    print("bpftest: ping replies WITH the filter:   "); pnum(after); print("\n");

    char st[256]; long n = sys_readfile("/proc/bpf", st, sizeof st - 1);
    if (n > 0) { st[n] = 0; print("/proc/bpf:\n"); print(st); }

    sys_sleep(20000);
    return 0;
}
