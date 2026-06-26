// threads.c — the full kernel-threads stack (M1138/M1139/M1140). Several threads
// share ONE address space and:
//   - hammer a shared counter, each increment guarded by a futex MUTEX
//     (a non-atomic counter++ that only stays exact if the mutex serializes them);
//   - are waited on with a real kernel JOIN (which also reaps them);
//   - each gets its OWN thread-local storage via a per-thread %fs base (TLS):
//     a thread writes its tid through %fs:0 and it lands in *its own* slot,
//     proving %fs is restored per-thread across preemption.
#include "ulib.h"

#define NTHREADS 4
#define PER      2000

static volatile int  counter = 0;          // shared by every thread
static volatile int  lock    = 0;          // the futex mutex word
static volatile long tls_slot[NTHREADS];   // thread i points its %fs base at tls_slot[i]
static int           spawn_tid[NTHREADS];

static void fs_set0(long v) { __asm__ volatile("movq %0, %%fs:0" :: "r"(v) : "memory"); }  // *(%fs+0) = v

static void worker(void *arg) {
    int i = (int)(long)arg;
    sys_set_tls((void *)&tls_slot[i]);      // per-thread TLS base
    fs_set0(sys_gettid());                  // write my tid via %fs:0 -> my own tls_slot[i]
    for (int k = 0; k < PER; k++) {
        mutex_lock(&lock);
        counter++;                          // non-atomic, made safe by the mutex
        mutex_unlock(&lock);
    }
}

static void pdec(long v) {
    char b[24]; int i = 0; unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    if (!u) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) b[i++] = '-';
    char o[24]; int j = 0; while (i) o[j++] = b[--i]; o[j] = 0; print(o);
}

int main(void) {
    sys_setcolor(4); print("threads:"); sys_setcolor(0); print(" main tid="); pdec(sys_gettid());
    print(" -- spawning "); pdec(NTHREADS); print(" threads (shared AS + mutex + join + TLS)\n\n");

    for (int i = 0; i < NTHREADS; i++) {
        spawn_tid[i] = thread_spawn(worker, (void *)(long)i);
        print("  spawned tid="); pdec(spawn_tid[i]); print("\n");
    }
    for (int i = 0; i < NTHREADS; i++) sys_join(spawn_tid[i]);   // wait + reap each

    print("\n"); sys_setcolor(4); print("mutex:"); sys_setcolor(0); print("  shared counter = ");
    sys_setcolor(9); pdec(counter); sys_setcolor(0);
    print("  (expected "); pdec((long)NTHREADS * PER); print(")\n");

    int tls_ok = 1;
    for (int i = 0; i < NTHREADS; i++) if (tls_slot[i] != spawn_tid[i]) tls_ok = 0;
    sys_setcolor(4); print("TLS:"); sys_setcolor(0); print("    each thread's fs:0 write landed in its own slot: ");
    if (tls_ok) { sys_setcolor(9); print("YES\n"); } else { sys_setcolor(2); print("NO\n"); }
    sys_setcolor(0);

    if (counter == NTHREADS * PER && tls_ok) { sys_setcolor(9); print("\nPASS: shared address space + futex mutex + join/reap + per-thread TLS all work.\n"); }
    else { sys_setcolor(2); print("\nFAIL\n"); }
    sys_setcolor(0);

    sys_sleep(20000);
    return 0;
}
