/*
 * ksyms.c — kernel symbol lookup + panic backtrace (M1078).
 *
 * The symbol TABLE (ksyms[]/ksyms_count) is generated at build time by
 * tools/gen_ksyms.sh and linked in separately (see the two-pass link in the
 * Makefile). This file is the RUNTIME: an address->name binary search and a
 * frame-pointer stack walker the fault handler calls so a kernel panic prints
 * "function+offset" frames instead of bare hex.
 */
#include "ksyms.h"
#include "console.h"

/* Reverse of ksym_lookup: resolve a symbol NAME to its address (0 if absent).
 * The table is sorted by address, not name, so this is a linear scan — fine for
 * the loadable-module loader (M1261), which resolves a handful of imports once. */
unsigned long ksym_addr(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < ksyms_count; i++) {
        const char *a = ksyms[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return ksyms[i].addr;
    }
    return 0;
}

const char *ksym_lookup(unsigned long addr, unsigned long *off_out) {
    if (ksyms_count <= 0) return 0;
    /* table is sorted ascending by addr: find the largest entry <= addr */
    int lo = 0, hi = ksyms_count - 1, best = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (ksyms[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return 0;
    if (off_out) *off_out = addr - ksyms[best].addr;
    return ksyms[best].name;
}

/* A frame pointer is only safe to dereference if it is inside the low,
 * identity-mapped kernel RAM and 8-byte aligned. Kernel stacks are kmalloc'd in
 * low physical RAM (well under 1 GiB even at -m 256M), so this bound keeps a
 * corrupt chain from faulting us a second time inside the panic handler. */
static int fp_ok(unsigned long fp) {
    return fp >= 0x1000 && fp < 0x40000000UL && (fp & 7) == 0;
}

void backtrace(unsigned long rip, unsigned long rbp) {
    unsigned long off;
    const char *name;

    kprintf("  call trace:\n");

    name = ksym_lookup(rip, &off);
    if (name) kprintf("    [0] %s+0x%lx\n", name, off);
    else      kprintf("    [0] %p\n", (void *)rip);

    unsigned long *fp = (unsigned long *)rbp;
    for (int depth = 1; depth <= 24 && fp_ok((unsigned long)fp); depth++) {
        unsigned long ret  = fp[1];          /* saved return address */
        unsigned long next = fp[0];          /* saved caller frame pointer */
        if (ret == 0) break;
        name = ksym_lookup(ret, &off);
        if (name) kprintf("    [%d] %s+0x%lx\n", depth, name, off);
        else      kprintf("    [%d] %p\n", depth, (void *)ret);
        if (next <= (unsigned long)fp) break;   /* frame pointers must climb the stack */
        fp = (unsigned long *)next;
    }
}
