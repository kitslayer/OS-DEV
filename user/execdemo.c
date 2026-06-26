// execdemo.c — the classic fork()+exec() spawn pattern (M1121). The parent forks
// (COW, M1116); the child replaces its own program image in place with exec(),
// becoming the 'clock' app — same process/window, a different program. Proof that
// exec swaps the image: the child's window runs a clock it never contained.
#include "ulib.h"

static void pnum(long v) {
    if (v < 0) { print("-"); v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v);
    char b[21]; int i = 0; while (n) b[i++] = t[--n]; b[i] = 0; print(b);
}

int main(void) {
    sys_setcolor(4); print("execdemo:"); sys_setcolor(0); print(" fork() + exec() -- the classic Unix spawn pattern\n");
    long pid = sys_fork();
    if (pid == 0) {
        // child: replace this image with the 'clock' program (same pid + window)
        sys_exec("clock", 0);
        print("child: exec failed!\n");        // unreachable on success
        sys_exit(1);
    }
    print("execdemo: forked child pid="); pnum(pid);
    print(", exec'd it into 'clock'\n");
    sys_setcolor(9); print("execdemo: that window now runs a DIFFERENT program in the SAME process.\n"); sys_setcolor(0);
    print("execdemo: I (the parent) am still execdemo.\n");
    sys_sleep(20000);
    return 0;
}
