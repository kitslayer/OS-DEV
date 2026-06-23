// scnotify.c — userspace syscall supervision (seccomp-notify, M1124). The parent
// (supervisor) intercepts the forked child's writefile() calls and decides their
// fate WITHOUT the child running the real syscall: first DENY (return -1), then
// EMULATE success (return a fabricated 4) — the child believes it wrote, but
// nothing touched the disk. This is userspace syscall interposition/emulation.
#include "ulib.h"
#include "syscall.h"            // SYS_writefile

static void pnum(long v) {
    if (v < 0) { print("-"); v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v);
    char b[21]; int i = 0; while (n) b[i++] = t[--n]; b[i] = 0; print(b);
}

int main(void) {
    long pid = sys_fork();
    if (pid == 0) {
        // ---- child: ask for my writefile() calls to be supervised ----
        sys_seccomp(SYS_writefile);
        long r1 = sys_writefile("/secret", "data", 4);   // -> parks; supervisor DENIES
        print("child: writefile #1 -> "); pnum(r1); print("  (supervisor denied it)\n");
        long r2 = sys_writefile("/secret", "data", 4);   // -> parks; supervisor EMULATES success
        print("child: writefile #2 -> "); pnum(r2); print("  (supervisor faked success)\n");
        sys_sleep(20000);
        sys_exit(0);
    }

    // ---- parent: the supervisor ----
    unsigned long ev[4];
    if (sys_seccomp_wait(pid, ev) > 0) {
        print("supervisor: child wants syscall #"); pnum((long)ev[0]); print(" -> DENY (return -1)\n");
        sys_seccomp_reply(pid, 0, -1);          // run_real=0, retval=-1
    }
    if (sys_seccomp_wait(pid, ev) > 0) {
        print("supervisor: child wants syscall #"); pnum((long)ev[0]); print(" -> EMULATE (return 4)\n");
        sys_seccomp_reply(pid, 0, 4);           // run_real=0, retval=4 (fabricated, no real write)
    }
    sys_sleep(20000);
    return 0;
}
