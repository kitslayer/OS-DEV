// crash.c — deliberately trigger a ring-3 fault, to demonstrate the kernel's
// ELF core dumper (M1104). On the fault, app_fault_current writes /tmp/core;
// inspect it afterwards with `file /tmp/core`.
#include "ulib.h"

int main(void) {
    print("crash: writing some recognizable values, then dereferencing NULL...\n");
    volatile long marker = 0xC0FFEE;        /* lands on the stack -> captured in the core */
    (void)marker;
    volatile int *p = (volatile int *)0;     /* the null pointer */
    *p = 0x1234;                             /* page fault -> kernel writes a core dump, kills us */
    print("crash: unreachable\n");
    return 0;
}
