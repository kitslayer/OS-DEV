// forktest.c — exercise copy-on-write fork() (M1116). The child is a clone of
// the parent's address space that shares physical pages until one side writes:
// a write splits the page, so the child's mutation is invisible to the parent.
//
// Parent and child each get their own window. The proof of COW isolation is that
// after the child sets the shared variable to 222, the PARENT still reads 111.
#include "ulib.h"

static void pnum(long v) {                 // printl is shell-local; print decimals ourselves
    if (v < 0) { print("-"); v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v);
    char b[21]; int i = 0; while (n) b[i++] = t[--n]; b[i] = 0; print(b);
}

static volatile int shared = 111;          // lives in writable .data -> a COW page after fork

int main(void) {
    print("forktest: parent, shared="); pnum(shared); print("\n");

    long pid = sys_fork();
    if (pid < 0) { print("forktest: fork failed\n"); sys_sleep(3000); return 1; }

    if (pid == 0) {
        // ---- child ----
        print("child: forked! I inherited shared="); pnum(shared); print("\n");
        shared = 222;                       // WRITE -> copy-on-write split (my private page now)
        print("child: I set shared=222 in MY copy; exiting\n");
        sys_sleep(8000);
        return 0;
    }

    // ---- parent ----
    print("parent: child pid="); pnum(pid); print("\n");
    sys_sleep(1200);                        // let the child run + write its copy
    print("parent: after the child wrote 222, MY shared is still=");
    pnum(shared);                            // expect 111 — COW kept the pages independent
    print(shared == 111 ? "  (COW isolation OK)\n" : "  (BROKEN: shared leaked!)\n");
    sys_sleep(20000);
    return 0;
}
