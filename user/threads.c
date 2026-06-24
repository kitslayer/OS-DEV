// threads.c — kernel threads + sync demo (M1138/M1139). Several threads share
// ONE address space and hammer a shared counter, but each increment is now a
// plain non-atomic counter++ guarded by a futex-backed MUTEX. If the mutex truly
// serializes them, the final count is exact (NTHREADS * PER); if it didn't, lost
// updates would make it short. The main thread then JOINs each worker (a real
// kernel wait-for-thread that also reaps it), instead of polling.
#include "ulib.h"

#define NTHREADS 4
#define PER      2000

static volatile int counter = 0;     // shared by every thread
static volatile int lock    = 0;     // the mutex word (0 = free, 1 = held)

static void worker(void *arg) {
    (void)arg;
    for (int i = 0; i < PER; i++) {
        mutex_lock(&lock);
        counter++;                   // a NON-atomic increment, made safe by the mutex
        mutex_unlock(&lock);
    }
    // falls off the end -> thread_exit_stub -> sys_thread_exit
}

static void pdec(long v) {
    char b[24]; int i = 0; unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    if (!u) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[i++] = '-';
    char o[24]; int j = 0; while (i) o[j++] = b[--i]; o[j] = 0; print(o);
}

int main(void) {
    print("threads: main tid="); pdec(sys_gettid()); print("\n");
    print("spawning "); pdec(NTHREADS); print(" threads; each does "); pdec(PER);
    print(" MUTEX-guarded increments of a shared counter.\n\n");

    int tid[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        tid[i] = thread_spawn(worker, 0);
        print("  spawned tid="); pdec(tid[i]); print("\n");
    }
    for (int i = 0; i < NTHREADS; i++) sys_join(tid[i]);   // wait for + reap each (no polling)

    print("\nall threads joined. shared counter = "); pdec(counter);
    print("  (expected "); pdec((long)NTHREADS * PER); print(")\n");
    print(counter == NTHREADS * PER
          ? "PASS: the futex mutex serialized the threads; join + reap worked.\n"
          : "FAIL: lost updates (mutex broken) or bad join.\n");

    sys_sleep(20000);
    return 0;
}
