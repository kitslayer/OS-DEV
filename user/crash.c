// crash.c — deliberately trigger a ring-3 fault, to exercise the kernel's ELF
// core dumper (M1104; on the fault app_fault_current writes /tmp/core — inspect
// it with `file /tmp/core`). With the launch arg "stack" it instead overflows
// the user stack, exercising the ring-3 stack guard page (M1499/M1500): the
// overflow must fault cleanly on the guard and kill only this app.
#include "ulib.h"

static volatile int g_go = 1;   /* volatile so the compiler can't fold the recursion to infinite */

// Recurse with a per-frame stack buffer until the user stack runs off its bottom
// into the unmapped guard page (M1499) -> a clean ring-3 #PF that kills only us.
static long blow(long d) {
    volatile char buf[1024];
    for (int i = 0; i < 1024; i++) buf[i] = (char)(d + i);
    long r = 0;
    if (g_go) r = blow(d + 1);
    return r + buf[(unsigned long)d & 1023];   /* use buf AFTER the call: not a tail call */
}

int main(void) {
    char arg[24];
    if (sys_getarg(arg, sizeof arg) > 0 && arg[0] == 's') {   /* `crash stack`: user-stack overflow */
        print("crash: overflowing the user stack (must fault on the guard page)...\n");
        volatile long sink = blow(0);
        (void)sink;                          /* unreachable: the recursion faults into the guard first */
        return 0;
    }
    print("crash: writing some recognizable values, then dereferencing NULL...\n");
    volatile long marker = 0xC0FFEE;         /* lands on the stack -> captured in the core */
    (void)marker;
    volatile int *p = (volatile int *)0;     /* the null pointer */
    *p = 0x1234;                             /* page fault -> kernel writes a core dump, kills us */
    print("crash: unreachable\n");
    return 0;
}
