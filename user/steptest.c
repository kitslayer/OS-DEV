// steptest.c — hardware single-step instruction trace (M1123). Arm the next few
// instructions to single-step (x86 TRAP flag + the #DB handler), run some plain
// arithmetic so there's something to step through, then read back the recorded
// instruction-pointer trace from /proc/self/sstrace. Each line is one instruction
// the CPU trapped on — the instruction-level complement to the syscall strace.
#include "ulib.h"

int main(void) {
    print("steptest: arming single-step for the next 8 instructions...\n");

    sys_singlestep(8);                    // the next 8 user instructions trap + get recorded
    volatile int x = 0;                   // <- these run under single-step
    x++; x++; x++; x++; x++; x++;         // plain arithmetic (no syscalls) to step through
    (void)x;

    // by now single-step has disarmed (8 instructions done); read the trace
    char buf[640];
    long n = sys_readfile("/proc/self/sstrace", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; print("steptest: instruction trace:\n"); print(buf); }
    else        print("steptest: no trace recorded\n");

    sys_sleep(20000);
    return 0;
}
