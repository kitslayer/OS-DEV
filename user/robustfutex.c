// robustfutex.c — robust mutex demo (M1141). A worker thread locks a mutex and
// then EXITS while still holding it (simulating a crash mid-critical-section).
// Without robust futexes the main thread would block on that lock forever; with
// them, the kernel walks the dead thread's robust list on exit, marks the lock
// FUTEX_OWNER_DIED, and wakes us — so rmutex_lock returns EOWNERDEAD and we
// recover ownership instead of hanging.
#include "ulib.h"

static volatile int m = 0;        // the robust mutex word (0 = free, else owner tid)
static volatile int locked = 0;   // the worker signals once it holds the lock
static robust_t worker_robust;    // the worker's robust list (global => durable for the exit walk)

static void pdec(long v) {
    char b[24]; int i = 0; unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    if (!u) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[i++] = '-';
    char o[24]; int j = 0; while (i) o[j++] = b[--i]; o[j] = 0; print(o);
}

static void dier(void *arg) {
    (void)arg;
    sys_set_robust_list(&worker_robust);      // tell the kernel where my held-locks list lives
    rmutex_lock(&m, &worker_robust);          // take the lock
    locked = 1;
    sys_thread_exit();                        // DIE holding it -- no unlock!
}

int main(void) {
    sys_setcolor(4); print("robust futex:"); sys_setcolor(0); print(" a worker locks a mutex then EXITS while holding it.\n");
    print("the kernel must release it (owner-died) so we can recover, not hang.\n\n");

    int tid = thread_spawn(dier, 0);
    print("spawned 'dier' thread tid="); pdec(tid); print("\n");
    while (!locked) sys_sleep(5);             // wait until it owns the lock
    print("dier now holds the lock and is exiting...\n");

    int rc = rmutex_lock(&m, 0);              // blocks until dier dies -> owner-died -> we wake + recover
    print("rmutex_lock returned "); pdec(rc);
    print(rc == 1 ? " (EOWNERDEAD -- recovered the dead owner's lock)\n" : " (0)\n");
    rmutex_unlock(&m, 0);
    sys_join(tid);                            // reap the (already-dead) worker

    if (rc == 1) { sys_setcolor(9); print("\nPASS: a thread died holding a lock; robust futex released + recovered it.\n"); }
    else { sys_setcolor(2); print("\nFAIL: owner death not detected (would have hung without robust futexes).\n"); }
    sys_setcolor(0);
    sys_sleep(20000);
    return 0;
}
