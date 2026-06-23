// futex.c — demonstrate a cross-process futex over named shared memory (M1109).
//   `futex wait` : map the shared word, spawn a `futex wake` child (which waits
//                  briefly so we register first), then FUTEX_WAIT — the child in
//                  a SEPARATE process wakes us. Prints "WOKEN" on success.
//   `futex wake` : map the same word, sleep briefly, then FUTEX_WAKE one waiter.
// One `futex wait` command does the whole cross-process block->wake.
#include "ulib.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

int main(void) {
    char arg[16]; sys_getarg(arg, sizeof arg);
    int *w = (int *)sys_shm_open("ftx", 4096);          // the shared futex word
    if (!w) { print("futex: shm_open failed\n"); sys_sleep(2000); return 1; }

    if (arg[0] == 'w' && arg[1] == 'o') {               // "wake" (the child process)
        sys_sleep(600);                                 // give the waiter time to register + block
        long n = sys_futex(w, FUTEX_WAKE, 1);
        char c[2] = { (char)('0' + (n < 0 ? 0 : n > 9 ? 9 : n)), 0 };
        print("futex(wake): woke "); print(c); print(" waiter(s) in another process\n");
        sys_sleep(3000);
        return 0;
    }

    // default: "wait"
    *w = 1;
    print("futex(wait): blocking on the shared word; a child process will wake me...\n");
    sys_spawn_arg("futex", "wake");                     // a SEPARATE process that will FUTEX_WAKE us
    long r = sys_futex(w, FUTEX_WAIT, 1);
    print(r == 0 ? "futex(wait): WOKEN by the other process! cross-process futex works.\n"
                 : "futex(wait): returned without blocking\n");
    sys_sleep(3000);                                    // keep the window up for the screenshot
    return 0;
}
