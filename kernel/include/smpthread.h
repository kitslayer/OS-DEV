/* smpthread.h — real, independently-progressing kernel threads pinned one-per-
 * core, distinct from smp_parallel_for's "split N work items, dispatch, join"
 * job model (kernel/smp.c).
 *
 * smp_parallel_for is for a single bounded batch of identical, short-lived pure
 * compute (every chunk finishes and the caller resumes together). An
 * smp_thread is a long-lived, independent kernel thread: it can loop, sleep,
 * yield to a sibling thread pinned to the SAME core, and finish on its own
 * schedule while OTHER threads (and the BSP) keep running — the "workloads
 * like multithreading programs" ask this exists to answer (M1530).
 *
 * Model, and why it's shaped this way:
 *   - A thread is pinned to ONE core for its whole life (assigned round-robin
 *     across the online APs at spawn time). No cross-core migration: that
 *     would need this thread's CR3/TSS/interrupt state to move too, which this
 *     kernel doesn't support yet for kernel threads. Pinning sidesteps all of
 *     that at the cost of no dynamic load-balancing.
 *   - Multiple threads CAN share one core: smp_thread_yield() cooperatively
 *     hands the core to the next READY sibling in that core's ring, round-
 *     robin, same shape as task.c's ring but per-core and separate from it.
 *     There is no PREEMPTION here (no per-core timer source exists yet) — a
 *     thread that never yields/returns monopolizes its core, same as any
 *     cooperative-fiber/green-thread model. Document this to callers, don't
 *     pretend it's preemptive.
 *   - Kernel-only: these run kernel C code in the kernel's own address space,
 *     never ring 3, so none of task.c's CR3-switch / TSS-rsp0 / FPU-save
 *     machinery is needed (the whole kernel builds -mgeneral-regs-only, so
 *     kernel C code never touches SSE/x87 state to begin with).
 *   - Deliberately a SEPARATE ring/lock from task.c's existing scheduler:
 *     zero changes to the single-core-assumed `current`/ready-ring the BSP's
 *     preemptive scheduler already depends on for ring-3 apps.
 */
#pragma once

typedef struct smp_thread smp_thread_t;
typedef void (*smp_thread_fn)(void *arg);

/* Spawn a thread running fn(arg) on the next (round-robin) online AP core.
 * Returns NULL and runs fn(arg) SYNCHRONOUSLY on this uniprocessor / at OOM --
 * callers that need to tell "ran inline" from "OOM, didn't run" apart should
 * check smp_cpu_count > 1 themselves first; for a fire-and-forget worker the
 * two cases need no distinction (the work either already happened or never
 * will, both are "done" from the caller's perspective). */
smp_thread_t *smp_thread_spawn(smp_thread_fn fn, void *arg);

/* Called BY a running smp_thread: give up this core to the next READY sibling
 * pinned here (round-robin), or return immediately if there is none. */
void smp_thread_yield(void);

/* Block (busy-poll) until `t` has finished, then free its stack. NULL-safe
 * (a NULL from smp_thread_spawn already means "done", nothing to join). */
void smp_thread_join(smp_thread_t *t);

/* Called from kernel/smp.c's ap_main() idle loop: run any smp_threads pinned
 * to THIS core to completion (or until they've all yielded/exited), then
 * return control to the caller's own idle loop. A no-op if nothing is pinned
 * here. */
void smpthread_ap_tick(void);
