// fswaittest.c — fswait multi-wait (M1125): block on a SET of name-VFS objects
// and learn which became ready, plus a timeout. The select/poll/epoll of OS-DEV,
// built for a no-fd-table design (it takes paths, not fds). The parent waits on
// two eventfds at once; the forked child signals one after a delay; fswait wakes
// and reports its index. Then a second wait times out with nobody signalling.
#include "ulib.h"

static void pnum(long v) {
    if (v < 0) { print("-"); v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = '0' + (int)(v % 10); v /= 10; } while (v);
    char b[21]; int i = 0; while (n) b[i++] = t[--n]; b[i] = 0; print(b);
}

int main(void) {
    const char *set[2] = { "/event/fa", "/event/fb" };

    long pid = sys_fork();
    if (pid == 0) {
        // child: after a beat, signal the SECOND event -> wakes the parent's fswait
        sys_sleep(800);
        sys_writefile("/event/fb", "1", 1);
        sys_sleep(20000);
        sys_exit(0);
    }

    // parent: block on BOTH events (neither ready yet) until the child signals one
    sys_setcolor(4); print("fswaittest:"); sys_setcolor(0); print(" waiting on /event/fa + /event/fb (up to 5s)...\n");
    long idx = sys_fswait(set, 2, 5000);
    if (idx < 0) { sys_setcolor(3); print("fswaittest: TIMEOUT\n"); sys_setcolor(0); }
    else { sys_setcolor(9); print("fswaittest: woke -> index "); pnum(idx); print(" ready ("); print(set[idx]); print(")\n"); sys_setcolor(0); }

    // timeout case: wait on an object nobody signals
    const char *none[1] = { "/event/never" };
    long t = sys_fswait(none, 1, 500);
    print("fswaittest: 2nd wait (nobody signals) returned "); pnum(t); print(" (expect -1 = timeout)\n");

    sys_sleep(20000);
    return 0;
}
