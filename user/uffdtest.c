// uffdtest.c — userfaultfd demo (M1134). A page that does not exist until it is
// read, and is then filled by a DIFFERENT process on demand.
//
// The parent mmap's a region and registers it for userfault handling, then
// forks. The child becomes the fault MONITOR: it blocks in sys_uffd_read() until
// the parent touches the region. When the parent reads the (unbacked) page, it
// FAULTS and parks inside the kernel; the child wakes, generates the page's
// contents, and sys_uffd_copy()'s them straight into the parent's address space,
// waking the parent — whose read now returns the just-materialized bytes. This
// is the primitive under live migration / post-copy / on-demand paging.
#include "ulib.h"

int main(void) {
    char *region = (char *)sys_mmap(4096);
    if (!region) { print("mmap failed\n"); sys_sleep(20000); return 1; }
    if (sys_uffd_register(region, 4096) < 0) { print("uffd_register failed\n"); sys_sleep(20000); return 1; }

    long pid = sys_fork();
    if (pid == 0) {
        // ---- child: the userfault monitor ----
        long addr = sys_uffd_read();                 // block until the parent faults
        if (addr > 0) {
            const char *msg =
                "[userfaultfd] this page did not exist until you read it -- a separate "
                "monitor process generated and supplied it on demand.";
            char buf[256]; int n = 0;
            while (msg[n] && n < 255) { buf[n] = msg[n]; n++; }
            buf[n] = 0;
            sys_uffd_copy((void *)addr, buf, (unsigned long)n + 1);
        }
        sys_exit(0);
    }

    // ---- parent: the faulter ----
    sys_sleep(300);                                  // let the child reach sys_uffd_read()
    print("faulter: reading an unbacked, userfault-registered page...\n");
    print("faulter: page contents -> ");
    sys_setcolor(9); print(region); sys_setcolor(0);   // first touch FAULTS -> serviced by the child (content lime)
    print("\n");
    int st; sys_waitpid((int)pid, &st);
    sys_setcolor(9); print("faulter: that page was materialized on demand, in another process, via userfaultfd.\n"); sys_setcolor(0);
    sys_sleep(20000);
    return 0;
}
