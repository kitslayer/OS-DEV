/*
 * sandbox.c — a live demo of the pledge() syscall sandbox (M1074).
 *
 * pledge() lets a process voluntarily drop the right to make whole CLASSES of
 * syscalls (stdio/rpath/wpath/inet/gfx/proc/vm/power). It is MONOTONIC — once
 * dropped, a promise can never be regained — and the moment the program calls a
 * syscall outside what it kept, the KERNEL terminates it (like OpenBSD's
 * SIGABRT). This app pledges "stdio", proves stdio still works, then deliberately
 * tries to write a file (the "wpath" class it dropped) so you can watch the
 * kernel kill it. (Check `dmesg` afterwards for the "[pledge] ... killing" line.)
 */
#include "ulib.h"

int main(void) {
    sys_setcolor(4); print("  == pledge()/unveil() sandbox demo ==\n\n"); sys_setcolor(0);

    /* --- unveil(): restrict the visible filesystem (a denied path fails, no kill) --- */
    print("unveil(\"/tmp\",\"rw\") -- only /tmp is now reachable\n");
    sys_unveil("/tmp", "rw");
    sys_unveil(0, 0);                       /* lock: no more unveils */

    if (sys_writefile("/tmp/SB.TXT", "hi from the sandbox\n", 20) >= 0)
        print("  write /tmp/SB.TXT: ok (under an unveiled prefix)\n");
    char buf[64];
    long rn = sys_readfile("README.TXT", buf, sizeof buf - 1);
    print(rn < 0 ? "  read README.TXT: DENIED by unveil (good)\n"
                 : "  read README.TXT: leaked! unveil FAILED\n");
    long rok = sys_readfile("/tmp/SB.TXT", buf, sizeof buf - 1);
    print(rok >= 0 ? "  read /tmp/SB.TXT: ok (it's unveiled)\n"
                   : "  read /tmp/SB.TXT: unexpectedly denied\n");
    print("\n");

    unsigned char rb[4];
    sys_getrandom(rb, sizeof rb);
    print("stdio call (getrandom) BEFORE pledge: ok\n\n");

    print("pledge(\"stdio\") -- dropping rpath/wpath/inet/proc/...\n");
    sys_pledge("stdio");

    if (sys_pledge("stdio wpath") < 0)
        print("pledge(\"stdio wpath\") REJECTED: promises only shrink. good.\n");

    sys_getrandom(rb, sizeof rb);
    print("stdio call (getrandom) AFTER pledge: still ok\n\n");

    sys_setcolor(2);
    print("Now trying a WPATH call (write a file) -- not pledged.\n");
    print("The kernel should terminate me right here...\n");
    sys_setcolor(0);
    sys_sleep(2500);                 /* leave the window readable for a moment */

    sys_writefile("PLEDGE.TXT", "should never be written", 23);   /* VIOLATION -> killed */

    sys_setcolor(3);
    print("\n!!! STILL ALIVE -- pledge FAILED !!!\n");   /* must never appear */
    for (;;) sys_sleep(1000);
    return 0;
}
