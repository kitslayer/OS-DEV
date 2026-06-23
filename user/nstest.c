// nstest.c — per-process mount namespaces (M1122). The parent forks (COW, M1116);
// the child unshare()s into a PRIVATE mount namespace, binds /scratch -> /tmp,
// and writes+reads a file through it. The parent, still in the shared namespace,
// can't see /scratch at all — the child's bind is invisible to it. fork + unshare
// + a private bind is exactly a container.
#include "ulib.h"

int main(void) {
    long pid = sys_fork();
    if (pid == 0) {
        // ---- child: detach the namespace, then bind privately ----
        if (sys_unshare() < 0) { print("child: unshare failed\n"); sys_exit(1); }
        sys_bind("/tmp", "/scratch");                       // /scratch -> /tmp, in MY namespace only
        sys_writefile("/scratch/ns.txt", "PRIVATE-NS", 10); // = /tmp/ns.txt via the bind
        char b[32]; long n = sys_readfile("/scratch/ns.txt", b, sizeof b - 1);
        if (n > 0) { b[n] = 0; print("child:  /scratch/ns.txt = "); print(b); print("  (my private bind works)\n"); }
        else        print("child:  /scratch read FAILED\n");
        sys_sleep(20000);
        sys_exit(0);
    }

    // ---- parent: still in the shared namespace; the child's bind must be invisible ----
    sys_sleep(1500);                                        // let the child unshare + bind + write
    char b[32]; long n = sys_readfile("/scratch/ns.txt", b, sizeof b - 1);
    if (n <= 0) print("parent: /scratch/ns.txt NOT FOUND  (the child's bind is isolated -- namespaces work!)\n");
    else        { b[n] = 0; print("parent: LEAKED: /scratch = "); print(b); print("\n"); }
    sys_sleep(20000);
    return 0;
}
