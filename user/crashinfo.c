// crashinfo.c — a userspace post-mortem reader for the ELF core dumps the kernel
// writes to /tmp/core on a fatal ring-3 fault (app_core_dump, M1104). It parses
// the PT_NOTE NT_PRSTATUS register block, prints a register dump, and produces a
// backtrace two ways: a frame-pointer (saved-rbp) walk through the PT_LOAD stack
// image, plus a heuristic scan of the stack for return-address-like values (which
// works even when the crashed program omitted frame pointers). A tiny crash
// debugger — closing the loop on the previously write-only core dumps. M1112.
//
// Usage: `crashinfo [PATH]`  (PATH defaults to /tmp/core; run `crash` first).
#include "ulib.h"

/* little-endian field reads from the core image */
static unsigned long rd64(const unsigned char *p) {
    unsigned long v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v;
}
static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

/* decimal print (printl is shell-local, not in ulib) */
static void pdec(long v) {
    if (v < 0) { print("-"); v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v);
    char b[21]; int i = 0; while (n) b[i++] = t[--n]; b[i] = 0; print(b);
}

/* "0x" + minimal-width hex (addresses/values stay short and readable) */
static void phex(unsigned long v) {
    if (!v) { print("0x0"); return; }
    char tmp[16]; int t = 0;
    while (v) { unsigned nyb = v & 0xF; tmp[t++] = nyb < 10 ? ('0' + nyb) : ('a' + nyb - 10); v >>= 4; }
    char b[19]; int i = 0; b[i++] = '0'; b[i++] = 'x';
    while (t) b[i++] = tmp[--t];
    b[i] = 0; print(b);
}

/* orient an address by the app's fixed memory map (no symbol table available) */
static const char *region_of(unsigned long a) {
    if (a == 0)                              return " <- NULL";
    if (a >= 0x40000000UL && a < 0x44000000UL) return " <- text";
    if (a >= 0x44000000UL && a < 0x50000000UL) return " <- heap";
    if (a >= 0x50000000UL && a < 0x50080000UL) return " <- stack";
    if (a >= 0x60000000UL && a < 0x80000000UL) return " <- mmap";
    if (a == 0x80000000UL)                   return " <- vdso";
    return "";
}
static int is_text(unsigned long a) { return a >= 0x40000000UL && a < 0x44000000UL; }

/* pointer to `need` bytes at virtual address `va` within some PT_LOAD, or 0 */
static const unsigned char *vaddr_ptr(const unsigned char *core, unsigned long csz,
                                      unsigned long phoff, int phnum,
                                      unsigned long va, unsigned long need) {
    for (int i = 0; i < phnum; i++) {
        const unsigned char *ph = core + phoff + (unsigned long)i * 56;
        if (rd32(ph) != 1) continue;                      /* PT_LOAD */
        unsigned long off = rd64(ph + 8), pva = rd64(ph + 16), fsz = rd64(ph + 32);
        if (va >= pva && need <= fsz && va - pva <= fsz - need) {
            unsigned long fo = off + (va - pva);
            if (fo + need <= csz) return core + fo;
        }
    }
    return 0;
}

int main(void) {
    char arg[96]; int al = sys_getarg(arg, sizeof arg - 1);
    if (al < 0) al = 0; arg[al] = 0;
    const char *path = al > 0 ? arg : "/tmp/core";

    unsigned long cap = 2u * 1024 * 1024;                 /* CORE_MAX */
    unsigned char *core = (unsigned char *)malloc(cap);
    if (!core) { print("crashinfo: out of memory\n"); sys_sleep(3000); return 1; }
    long n = sys_readfile(path, core, cap);
    if (n <= 0) {
        print("crashinfo: cannot read "); print(path);
        print("\n  (run 'crash' first to generate /tmp/core)\n");
        sys_sleep(5000); return 1;
    }
    unsigned long csz = (unsigned long)n;

    if (csz < 64 || core[0] != 0x7F || core[1] != 'E' || core[2] != 'L' || core[3] != 'F' || rd16(core + 16) != 4) {
        print("crashinfo: "); print(path); print(" is not an ELF core dump\n");
        sys_sleep(5000); return 1;
    }
    unsigned long phoff = rd64(core + 32);
    int phnum = (int)rd16(core + 56);
    print("crashinfo: "); print(path); print("  ("); pdec((long)csz);
    print(" bytes, "); pdec(phnum); print(" segments)\n");

    /* PT_NOTE -> NT_PRSTATUS: 27 GP regs at desc offset 112 */
    unsigned long regs[27]; int haveregs = 0;
    for (int i = 0; i < phnum && !haveregs; i++) {
        const unsigned char *ph = core + phoff + (unsigned long)i * 56;
        if (rd32(ph) != 4) continue;                      /* PT_NOTE */
        unsigned long o = rd64(ph + 8), end = o + rd64(ph + 32);
        while (o + 12 <= end && o + 12 <= csz) {
            unsigned namesz = rd32(core + o), descsz = rd32(core + o + 4), type = rd32(core + o + 8);
            unsigned long namepad = (namesz + 3u) & ~3u, descpad = (descsz + 3u) & ~3u;
            unsigned long desc = o + 12 + namepad;
            if (type == 1 && desc + 112 + 27 * 8 <= csz) { /* NT_PRSTATUS */
                for (int k = 0; k < 27; k++) regs[k] = rd64(core + desc + 112 + (unsigned long)k * 8);
                haveregs = 1; break;
            }
            o = o + 12 + namepad + descpad;
        }
    }
    if (!haveregs) { print("crashinfo: no NT_PRSTATUS register block found\n"); sys_sleep(5000); return 1; }

    /* user_regs_struct order: r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi orig_rax rip cs rflags rsp ss */
    unsigned long rbp = regs[4], rip = regs[16], rsp = regs[19], rflags = regs[18];

    print("\nfaulted at RIP="); phex(rip); print(region_of(rip)); print("\n");
    print("registers:\n");
    static const struct { const char *nm; int idx; } R[16] = {
        {"rax", 10}, {"rbx", 5}, {"rcx", 11}, {"rdx", 12}, {"rsi", 13}, {"rdi", 14},
        {"rbp", 4},  {"rsp", 19},{"r8 ", 9},  {"r9 ", 8},  {"r10", 7},  {"r11", 6},
        {"r12", 3},  {"r13", 2}, {"r14", 1},  {"r15", 0},
    };
    for (int i = 0; i < 16; i += 2) {
        print("  "); print(R[i].nm);   print("="); phex(regs[R[i].idx]);
        print("  "); print(R[i + 1].nm); print("="); phex(regs[R[i + 1].idx]); print("\n");
    }
    print("  rflags="); phex(rflags); print("\n");

    /* backtrace #1: frame-pointer (saved-rbp) walk */
    print("\nbacktrace (frame-pointer walk):\n");
    print("  #0  "); phex(rip); print(region_of(rip)); print("\n");
    unsigned long fp = rbp; int depth = 1;
    for (; depth <= 32 && fp; depth++) {
        const unsigned char *fr = vaddr_ptr(core, csz, phoff, phnum, fp, 16);
        if (!fr) break;
        unsigned long saved = rd64(fr), ret = rd64(fr + 8);
        if (!ret) break;
        print("  #"); pdec(depth); print("  "); phex(ret); print(region_of(ret)); print("\n");
        if (saved <= fp) break;                           /* must ascend; guards loops/garbage */
        fp = saved;
    }
    if (depth == 1) print("  (frame pointers omitted by the crashed program)\n");

    /* backtrace #2: heuristic — text-range values lying on the live stack */
    print("\ntext addresses on the stack (heuristic, RSP up):\n");
    unsigned long top = 0x50000000UL + 0x80000UL;         /* USTACK_BASE + 128 pages */
    int shown = 0;
    for (unsigned long a = rsp & ~7UL; a + 8 <= top && shown < 12; a += 8) {
        const unsigned char *p = vaddr_ptr(core, csz, phoff, phnum, a, 8);
        if (!p) continue;
        unsigned long v = rd64(p);
        if (is_text(v)) { print("  "); phex(a); print(": "); phex(v); print(" <- text\n"); shown++; }
    }
    if (!shown) print("  (none captured)\n");

    sys_sleep(25000);
    return 0;
}
