// msealtest.c — mseal: irreversible memory sealing (M1130, Linux 2024 mseal(2)).
//
// The W^X / JIT hardening story. We hand-assemble a tiny function into an mmap'd
// page, flip the page to R-X under W^X (so it can't be written and executed at
// once), run it, then SEAL it. After the seal, a hostile mprotect (make it
// writable again, to inject code) and a hostile munmap (remap a fresh page in
// its place) are BOTH refused by the kernel — the mapping is frozen for life.
// A second, unsealed region shows the seal is per-region, not global.
#include "ulib.h"

static void pdec(long v) {
    char b[24]; int i = 0, neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    if (!u) b[i++] = '0';
    while (u) { b[i++] = '0' + (u % 10); u /= 10; }
    if (neg) b[i++] = '-';
    char o[24]; int j = 0; while (i) o[j++] = b[--i]; o[j] = 0; print(o);
}
static void line(const char *what, long rc, const char *good) {
    print(what); print(" -> "); pdec(rc);
    print("  ("); print(good); print(")\n");
}

#define PROT_R 1
#define PROT_W 2
#define PROT_X 4

int main(void) {
    sys_setcolor(4); print("mseal:"); sys_setcolor(0); print(" irreversible memory sealing (W^X / JIT hardening)\n\n");

    // --- build a tiny JIT function in a fresh page ---
    unsigned char *code = (unsigned char *)sys_mmap(4096);
    if (!code) { print("mmap failed\n"); sys_sleep(20000); return 1; }
    // mov eax, 42 ; ret   (System V: returns 42)
    code[0] = 0xB8; code[1] = 0x2A; code[2] = 0x00; code[3] = 0x00; code[4] = 0x00; code[5] = 0xC3;

    line("mprotect(code, R-X)  W^X flip", sys_mprotect(code, 4096, PROT_R | PROT_X), "0 = ok");
    int (*fn)(void) = (int (*)(void))code;
    print("call the JIT'd code   -> "); pdec(fn()); print("  (42 = it ran)\n");

    long sealed = sys_mseal(code, 4096);
    line("mseal(code)          freeze it", sealed, ">=1 = regions sealed");

    print("\n-- now an attacker with an mprotect/munmap primitive tries to tamper --\n");
    line("mprotect(code, R-W)  re-arm write", sys_mprotect(code, 4096, PROT_R | PROT_W), "-1 = DENIED, sealed");
    line("munmap(code)         swap a page", sys_munmap(code, 4096),                      "-1 = DENIED, sealed");
    print("the sealed code is still 42: "); pdec(fn()); print(" (intact & immutable)\n");

    print("\n-- a different, UNSEALED region is still fully mutable --\n");
    unsigned char *scratch = (unsigned char *)sys_mmap(4096);
    scratch[0] = 1;                                   // touch (fault it in)
    line("mprotect(scratch, R-W)", sys_mprotect(scratch, 4096, PROT_R | PROT_W), "0 = ok, unsealed");
    line("munmap(scratch)", sys_munmap(scratch, 4096),                           "0 = ok, unsealed");

    sys_setcolor(9); print("\nseal is per-region and irreversible. that's mseal.\n"); sys_setcolor(0);
    sys_sleep(20000);
    return 0;
}
