// threads.c — kernel threads demo (M1138). Spawn several threads that all share
// ONE address space (unlike fork, which gives each child a separate copy), and
// have them hammer a single shared counter with hardware-atomic increments. If
// the address space is truly shared and the `lock`-prefixed increments are
// atomic across preemption, the final count is exact (NTHREADS * PER).
#include "ulib.h"

#define NTHREADS 4
#define PER      2000

static volatile int counter = 0;     // shared by every thread (one address space)
static volatile int done    = 0;

static void atomic_inc(volatile int *p) { __asm__ volatile("lock incl %0" : "+m"(*p)); }

static void worker(void *arg) {
    (void)arg;
    for (int i = 0; i < PER; i++) atomic_inc(&counter);   // racing on the SAME memory
    atomic_inc(&done);
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
    print("threads: main thread tid="); pdec(sys_gettid()); print("\n");
    print("spawning "); pdec(NTHREADS); print(" threads that SHARE one address space;\n");
    print("each does "); pdec(PER); print(" lock-atomic increments of a shared counter.\n\n");

    for (int i = 0; i < NTHREADS; i++) {
        int tid = thread_spawn(worker, (void *)(long)i);
        print("  spawned thread tid="); pdec(tid); print("\n");
    }

    int spins = 0;                                   // poll-join: wait for every worker to finish
    while (done < NTHREADS && spins < 800) { sys_sleep(10); spins++; }
    sys_sleep(300);                                  // let the workers fully reach sys_thread_exit

    print("\nshared counter = "); pdec(counter);
    print("  (expected "); pdec((long)NTHREADS * PER); print(")\n");
    print(counter == NTHREADS * PER
          ? "PASS: threads shared the address space and the atomics held under preemption.\n"
          : "FAIL: lost updates or unshared memory.\n");

    sys_sleep(20000);
    return 0;
}
