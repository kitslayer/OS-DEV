// sigfdtest.c — signalfd + the signal bitset (M1126). Route two signals to
// /proc/self/sigfd (no handler), raise BOTH back-to-back, then read them as a
// file. Both arrive — proving the new bitset preserves a second pending signal
// where the old one-deep slot would have dropped the first. Signals as a file,
// composable with fswait (M1125).
#include "ulib.h"
#define SIGUSR1 10
#define SIGUSR2 12

int main(void) {
    sys_signalfd((1u << SIGUSR1) | (1u << SIGUSR2));   // deliver these via /proc/self/sigfd

    sys_raise(SIGUSR1);    // queue signal 10
    sys_raise(SIGUSR2);    // queue signal 12 too -- the old single slot would have dropped #10

    char b[16];
    long n = sys_readfile("/proc/self/sigfd", b, sizeof b - 1);
    int ok1 = n > 0; if (ok1) { b[n] = 0; print("sigfd read #1: signo "); print(b); }
    long n2 = sys_readfile("/proc/self/sigfd", b, sizeof b - 1);
    int ok2 = n2 > 0; if (ok2) { b[n2] = 0; print("sigfd read #2: signo "); print(b); }
    if (ok1 && ok2) print("both signals arrived -> the bitset kept them (old one-deep slot would drop one)\n");
    else print("sigfdtest: FAILED -- a signal was dropped\n");   /* was unconditional -- the exact regression this test exists to catch would have still printed success */

    sys_sleep(20000);
    return 0;
}
