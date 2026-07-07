/*
 * init.c — the first userspace program.
 *
 * This is compiled and linked as a *separate* freestanding ELF executable
 * (base 0x40000000, see user/user.ld), then embedded into the kernel image.
 * It has no libc and no kernel headers beyond the shared syscall numbers — it
 * can only touch the world through `int 0x80`. It runs in ring 3.
 */
#include "syscall.h"

/* Make a system call: number in rax, args in rdi/rsi/rdx, result in rax. */
static long do_syscall(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "memory");
    return ret;
}

static unsigned long ustrlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void uwrite(const char *s) {
    do_syscall(SYS_write, 1, (long)s, (long)ustrlen(s));
}

void _start(void) {
    uwrite("Hello from userspace! I am running in ring 3.\n");
    uwrite("The only way I can reach the kernel is through syscalls.\n");

    long pid = do_syscall(SYS_getpid, 0, 0, 0);
    char pidbuf[24]; int pn = 0;               /* was buf[10]='0'+(pid%10): only the last digit -- harmless
                                                 * today since init is provably always pid 1, but wrong for
                                                 * any pid >= 10 as written */
    if (pid == 0) pidbuf[pn++] = '0';
    else { char t[24]; int tn = 0; long v = pid; while (v > 0) { t[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) pidbuf[pn++] = t[--tn]; }
    pidbuf[pn] = 0;
    uwrite("my pid is ");
    uwrite(pidbuf);
    uwrite("\n");

    uwrite("Calling exit(7).\n");
    do_syscall(SYS_exit, 7, 0, 0);

    for (;;) { }   /* exit does not return; this is just in case */
}
