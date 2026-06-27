/*
 * pietest.c — a position-independent executable, the test for load-time PIE
 * support (M1465). Built `-fPIE -pie` so it's an ET_DYN with R_X86_64_RELATIVE
 * relocations, NOT a fixed-address ET_EXEC like every other app.
 *
 * The point of the test: `gp` is a global pointer initialised to `&msg`. In a
 * PIE that absolute address is unknown until load time, so the linker emits one
 * R_X86_64_RELATIVE reloc for it. If the kernel (elf_load_dyn) loaded us at a
 * base AND applied that reloc, `gp` points at `msg` and we print the success
 * line; if the reloc were skipped, `gp` would hold its link-time value and the
 * dereference would fault. So a clean print == PIE loading + relocation work.
 *
 * Self-contained (no ulib) so the image carries ONLY a RELATIVE reloc — exactly
 * the case elf_load_dyn handles — which keeps this a clean test of the loader.
 *
 * Launch: `run pietest` from the shell.
 */

/* the OS syscall ABI: int 0x80, rax=number, rdi/rsi/rdx=args (see user/ulib.c) */
static long sys_write(int fd, const void *buf, unsigned long n) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(1L), "D"((long)fd), "S"((long)buf), "d"((long)n) : "memory");
    return r;
}
static void sys_exit(int code) {
    __asm__ volatile("int $0x80" :: "a"(2L), "D"((long)code) : "memory");
    for (;;) {}
}
/* SYS_writefile = 12 (name, buf, len): persist the result so it can be `cat`ed,
 * since a detached `run` app's stdout isn't echoed back into the shell. */
static long sys_writefile(const char *name, const void *buf, unsigned long n) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(12L), "D"((long)name), "S"((long)buf), "d"((long)n) : "memory");
    return r;
}

static const char msg[] = "pietest: PIE loaded + relocated OK\n";
/* `volatile` so -O2 can't fold gp into a RIP-relative ref to msg and delete the
 * pointer: it must keep gp as real storage initialised to &msg, which is the
 * absolute address that needs the R_X86_64_RELATIVE fix-up at load time. */
static const char * volatile gp = msg;

void _start(void) {
    unsigned long n = 0;
    while (gp[n]) n++;                 /* strlen via the relocated pointer (faults here if the reloc was skipped) */
    sys_write(1, gp, n);              /* console (not echoed back by a detached `run`) */
    sys_writefile("PIEOUT.TXT", gp, n);   /* verifiable: `cat pieout.txt` shows the relocated string */
    sys_exit(0);
}
