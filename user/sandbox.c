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
    sys_setcolor(4); print("  == pledge() sandbox demo ==\n\n"); sys_setcolor(0);
    print("A program can drop the right to whole classes of syscalls.\n");
    print("Then the KERNEL kills it the instant it tries one.\n\n");

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
