/*
 * task.c — kernel threads and a round-robin scheduler.
 *
 * Tasks live in a circular linked list (the "ready ring"). `current` points at
 * the running one; scheduling just walks to `current->next`. A context switch
 * (context_switch.asm) swaps the saved stack pointer, which swaps everything a
 * thread is doing.
 *
 * This is *preemptive* round-robin: the timer IRQ calls sched_tick() (see
 * timer.c), which switches to current->next, so a thread can be suspended
 * between any two instructions — threads also yield voluntarily via task_yield()
 * or block via task_block(). Because switches can happen anywhere, code that
 * shares state between tasks must guard it (e.g. irq_save/irq_restore below).
 * See docs/07 (threads) and docs/11 (preemption) for the history.
 */
#include "task.h"
#include "syscall.h"   /* SCHED_OTHER/FIFO/RR (M1172) */
#include "kheap.h"
#include "interrupts.h"
#include "gdt.h"
#include "string.h"
#include "timer.h"

#define STACK_SIZE 16384
#define FXSZ       512                 /* FXSAVE area size */

static task_t *current;
static int     next_id;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void fpu_save(void *area16);            /* FXSAVE  (kernel/asm/fpu.asm) */
extern void fpu_restore(const void *area16);   /* FXRSTOR */
extern uint8_t fpu_template[];                 /* a clean FP state, captured at boot */

/* 16-byte-aligned FXSAVE pointer inside a task's over-allocated fxbuf. */
static inline void *fxptr(task_t *t) {
    return (void *)(((uintptr_t)t->fxbuf + 15) & ~(uintptr_t)15);
}
static void fx_alloc(task_t *t) {              /* give a task its own FP save area */
    t->fxbuf = kmalloc(FXSZ + 16);
    if (t->fxbuf) memcpy(fxptr(t), fpu_template, FXSZ);
}

/* Copy the FP/SSE state of `src` (which must be the running task) into `dst` —
 * for fork(), so a child of a process mid-float-computation inherits its state.
 * fpu_save captures the live CPU FP state into src's area first. */
void task_copy_fpu(task_t *dst, task_t *src) {
    if (!dst || !src || !dst->fxbuf || !src->fxbuf) return;
    fpu_save(fxptr(src));                      /* src is current: capture its live FP state */
    memcpy(fxptr(dst), fxptr(src), FXSZ);
}

struct registers *task_uframe(task_t *t) { return t ? t->uframe : 0; }

static uint64_t active_cr3;     /* the address space currently loaded in CR3 */
static task_t *idle_task;       /* the scheduling floor: never blocks/exits, run ONLY when no other task is runnable (so a task that blocks itself when nothing else is ready hands off to this instead of spinning marked-BLOCKED). NULL until created -> switch_to_next falls back to its prior behavior. */

/* --- CFS weighted-fair scheduling (M1171) -------------------------------------
 * Each task carries a vruntime (weighted CPU consumed) and a weight derived from
 * its nice level; switch_to_next runs the runnable task with the SMALLEST
 * vruntime, and charges the running task slice*NICE0_WEIGHT/weight of vruntime,
 * so a low-nice (high-weight) task accrues vruntime slowly and thus gets more
 * CPU. With every task at nice 0 (weight 1024) vruntime advances by real time
 * for all, so the pick reduces to "least-recently-run" — fair round-robin,
 * preserving the prior behavior. The Linux nice->weight table (nice -20..+19). */
#define NICE0_WEIGHT 1024u
static const uint32_t sched_weight[40] = {  /* index = nice + 20 */
    88761, 71755, 56483, 46273, 36291,      /* -20..-16 */
    29154, 23254, 18705, 14949, 11916,      /* -15..-11 */
     9548,  7620,  6100,  4904,  3906,      /* -10..-6  */
     3121,  2501,  1991,  1586,  1277,      /* -5..-1   */
     1024,   820,   655,   526,   423,      /*  0..+4   */
      335,   272,   215,   172,   137,      /* +5..+9   */
      110,    87,    70,    56,    45,      /* +10..+14 */
       36,    29,    23,    18,    15 };     /* +15..+19 */
static uint32_t nice_to_weight(int nice) {
    if (nice < -20) nice = -20; if (nice > 19) nice = 19;
    return sched_weight[nice + 20];
}
static uint64_t g_min_vruntime;   /* floor: the smallest vruntime among runnable tasks (monotonic non-decreasing) */
#define RR_QUANTUM 5              /* SCHED_RR timeslice in timer ticks before rotating among equal-priority RR tasks (M1172) */
static void sched_place_wake(task_t *t) {   /* a waking task can't be below the floor (no unfair head start; no starvation) */
    if (t && t->vruntime < g_min_vruntime) t->vruntime = g_min_vruntime;
}

static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void load_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

/* Per-thread %fs base for TLS (M1140). The kernel never uses FS_BASE itself, so
 * we only touch the MSR on behalf of threads that set one; `loaded_fs_base`
 * tracks the live value so a system with no TLS pays nothing (stays 0 == 0). */
#define MSR_FS_BASE 0xC0000100u
static uint64_t loaded_fs_base = 0;
static void load_fs_base(uint64_t b) {
    if (b == loaded_fs_base) return;
    __asm__ volatile("wrmsr" : : "c"(MSR_FS_BASE), "a"((uint32_t)b), "d"((uint32_t)(b >> 32)));
    loaded_fs_base = b;
}
/* Set the CURRENT thread's TLS base (live + saved for restore). M1140. */
void task_set_fs_base(uint64_t b) { current->fs_base = b; load_fs_base(b); }

/* Register the CURRENT thread's userspace robust-futex list (M1141). */
void task_set_robust(uint64_t r) { current->robust = r; }
uint64_t task_robust(void) { return current->robust; }

/* Save RFLAGS and disable interrupts (returns old flags); and restore them.
 * Scheduling edits the shared ready ring, so it must be uninterruptible. */
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory", "cc");
}

/* Where a freshly created thread begins. `current` is already this thread. */
static void thread_trampoline(void) {
    interrupts_enable();        /* new threads start with interrupts on */
    current->entry();
    task_exit();                /* if the entry function returns, end cleanly */
}

/* The idle task: a guaranteed-runnable floor. Halts with interrupts on so the
 * timer can preempt it the instant any real task becomes runnable. */
static void idle_loop(void) { for (;;) __asm__ volatile("sti; hlt"); }

void sched_init(void) {
    task_t *t = kzalloc(sizeof(task_t));
    t->id = next_id++;
    t->state = TASK_RUNNING;
    t->next = t;                /* a ring of one */
    t->weight = NICE0_WEIGHT;   /* CFS: task 0 at nice 0 (M1171) — never leave weight 0 (div-by-zero in the charge) */
    t->cr3 = read_cr3();        /* task 0 runs in the kernel's address space */
    active_cr3 = t->cr3;
    fx_alloc(t);
    t->last_in = timer_ms();    /* start CPU-time accounting for task 0 */
    current = t;
    idle_task = task_create(idle_loop, read_cr3(), 0);   /* add the always-runnable floor (heap is up: kheap_init precedes sched_init) */
}

task_t *task_create(void (*entry)(void), uint64_t cr3, void *proc) {
    return task_create_stack(entry, cr3, proc, STACK_SIZE);
}

/* Like task_create but with an explicit kernel-stack size — for tasks that run
 * deep/heavy code (e.g. the browser fetch worker, which does bignum/RSA crypto
 * that would overflow the default 16 KB stack). */
task_t *task_create_stack(void (*entry)(void), uint64_t cr3, void *proc, int stack_size) {
    task_t *t = kzalloc(sizeof(task_t));
    if (!t) return 0;                        /* OOM: fail cleanly rather than deref NULL */
    t->id = next_id++;
    t->entry = entry;
    t->state = TASK_READY;
    t->weight = NICE0_WEIGHT;   /* CFS: born at nice 0 (M1171) — must be non-zero before any vruntime charge */
    t->vruntime = g_min_vruntime;  /* start at the floor: don't dominate (vruntime 0) or starve */
    t->cr3 = cr3;             /* set BEFORE the ring insert: no startup race */
    t->proc = proc;

    fx_alloc(t);

    uint8_t *stack = kmalloc(stack_size);
    if (!stack || !t->fxbuf) {               /* OOM on the stack or FP save area: don't build a bogus stack frame
                                              * (a NULL stack -> top at a wild low address -> triple-fault on first
                                              * switch-in) or run with no FP save (XMM/x87 corruption between tasks). */
        if (stack) kfree(stack);
        if (t->fxbuf) kfree(t->fxbuf);
        kfree(t);
        return 0;
    }
    t->stack_base = (uint64_t)stack;

    /* Build the initial stack so the first switch-in "returns" into the
     * trampoline. The layout must mirror context_switch's pops:
     * (top) [trampoline][rbx][rbp][r12][r13][r14][r15] (rsp) */
    uint64_t top = ((uint64_t)stack + stack_size) & ~15ull;
    t->kstack_top = top;                   /* used as TSS rsp0 if this runs ring 3 */
    uint64_t *sp = (uint64_t *)top;
    *--sp = (uint64_t)thread_trampoline;   /* return address for `ret` */
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */
    t->rsp = (uint64_t)sp;

    /* Insert into the ring right after the current task. */
    uint64_t f = irq_save();
    t->next = current->next;
    current->next = t;
    irq_restore(f);
    return t;
}

/* Core switch — assumes interrupts already disabled. */
static void switch_to_next(void) {
    task_t *prev = current;
    uint64_t now = timer_ms();

    /* Charge prev for the slice it just ran BEFORE deciding (M1171): real time to
     * run_ms, and weighted time to vruntime, so the comparison below sees prev's
     * up-to-date cost (otherwise a still-running task's stale vruntime would let
     * it win forever). Done every call — including the keep-prev early return. */
    uint64_t slice = now - prev->last_in;
    prev->last_in = now;
    prev->run_ms += slice;
    if (prev != idle_task)
        prev->vruntime += slice * NICE0_WEIGHT / (prev->weight ? prev->weight : NICE0_WEIGHT);
    if (prev->policy == SCHED_RR && prev->rt_ticks > 0) prev->rt_ticks--;   /* RR timeslice tick (M1172) */

    task_t *best = 0;
    /* --- Real-time classes first (M1172): the highest-priority runnable FIFO/RR
       task preempts the ENTIRE normal (CFS) class. Only if none are runnable do
       we fall through to the CFS pick — so with no RT task this is byte-for-byte
       the M1171 behavior. --- */
    int top_rt = -1;
    for (task_t *u = prev->next; ; u = u->next) {
        if (u != idle_task && (u->state == TASK_RUNNING || u->state == TASK_READY)
            && u->policy != SCHED_OTHER && u->rt_priority > top_rt)
            top_rt = u->rt_priority;
        if (u == prev) break;
    }
    if (top_rt >= 0) {                          /* an RT task wants the CPU */
        int prev_top = (prev != idle_task && (prev->state == TASK_RUNNING || prev->state == TASK_READY)
                        && prev->policy != SCHED_OTHER && prev->rt_priority == top_rt);
        if (prev_top && !(prev->policy == SCHED_RR && prev->rt_ticks <= 0)) {
            best = prev;                        /* incumbent keeps the CPU: FIFO always; RR within its quantum */
        } else {
            for (task_t *u = prev->next; ; u = u->next) {   /* next runnable RT task at top_rt (ring order => RR rotation) */
                if (u != idle_task && (u->state == TASK_RUNNING || u->state == TASK_READY)
                    && u->policy != SCHED_OTHER && u->rt_priority == top_rt) { best = u; break; }
                if (u == prev) break;
            }
            if (!best) best = prev;
            if (prev_top) prev->rt_ticks = RR_QUANTUM;                          /* rotated out: refill */
            if (best && best != prev && best->policy == SCHED_RR) best->rt_ticks = RR_QUANTUM;  /* fresh quantum on switch-in */
        }
    } else {
        /* CFS pick: the runnable, non-idle task with the smallest vruntime. */
        task_t *t = prev;
        do {
            if (t != idle_task && (t->state == TASK_RUNNING || t->state == TASK_READY))
                if (!best || t->vruntime < best->vruntime) best = t;
            t = t->next;
        } while (t != prev);
        /* Advance the floor every call (even when we keep prev): it's the smallest
         * runnable vruntime, so a task that wakes mid-way clamps to HERE and can't
         * monopolize the CPU catching up to a long-running solo task (M1171). */
        if (best) g_min_vruntime = best->vruntime;
    }

    task_t *next;
    if (best == prev) return;                   /* prev is the most-deserving runnable -> keep it (no switch) */
    if (best) { next = best; }                  /* a different task has a smaller vruntime -> run it */
    else {                                       /* nothing non-idle is runnable */
        if (prev->state == TASK_RUNNING || prev->state == TASK_READY)
            return;                             /* prev still runnable (e.g. the desktop) -> keep it */
        if (!idle_task || prev == idle_task)
            return;                             /* no floor / already on it */
        next = idle_task;                       /* prev blocked/exited, nothing ready -> the idle floor */
    }

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        prev->ready_since = now;                 /* entered the run queue: start clocking its wait (M1148) */
        prev->nivcsw++;                           /* still runnable when switched out => preempted (M1150) */
    } else {
        prev->nvcsw++;                            /* it blocked/yielded/exited itself => voluntary (M1150) */
    }
    next->state = TASK_RUNNING;
    if (next->ready_since) {                      /* leaving the run queue: charge the time it waited (M1148) */
        next->rq_wait_ms += now - next->ready_since;
        next->ready_since = 0;
    }
    current = next;

    next->last_in = now;                          /* stamp switch-in (prev was already charged above) */
    next->nswitch++;

    /* switch address space if the next task lives in a different one. Safe to
     * do here: kernel code, this stack (heap), and the GDT/IDT/TSS are mapped
     * in every address space, so execution continues seamlessly. */
    if (next->cr3 && next->cr3 != active_cr3) {
        active_cr3 = next->cr3;
        load_cr3(next->cr3);
    }
    if (next->kstack_top)
        tss_set_rsp0(next->kstack_top);     /* traps from ring 3 land here */
    if (prev->fxbuf) fpu_save(fxptr(prev));     /* preserve FP/SSE across the switch */
    if (next->fxbuf) fpu_restore(fxptr(next));
    load_fs_base(next->fs_base);                 /* restore the thread's TLS base (M1140) */
    context_switch(&prev->rsp, next->rsp);
}

void schedule(void) {
    switch_to_next();
}

/* Preemption entry point, called from the timer IRQ. We're already in
 * interrupt context (IF clear), so we can switch directly. switch_to_next is a
 * no-op until sched_init runs (current == NULL) or when only one task exists. */
void sched_tick(void) {
    if (current)
        switch_to_next();
}

void task_yield(void) {
    uint64_t f = irq_save();
    switch_to_next();
    irq_restore(f);
}

/* Block the current task: it leaves the run rotation until task_wake() marks it
 * runnable. Interrupts are off across the state change + switch so a wakeup
 * can't be lost. */
void task_block(void) {
    uint64_t f = irq_save();
    current->wchan = (uint64_t)__builtin_return_address(0);   /* who we're blocking in, for /proc/sched WCHAN (M1166) */
    current->wake_at = 0;          /* not a timed sleep -> the timer scan must ignore it */
    current->state = TASK_BLOCKED;
    switch_to_next();
    irq_restore(f);
}

void task_wake(task_t *t) {
    if (t && t->state == TASK_BLOCKED) {
        t->wake_at = 0;            /* cancel any pending timed wake */
        t->state = TASK_READY;
        t->ready_since = timer_ms();   /* re-entered the run queue (M1148) */
        sched_place_wake(t);           /* clamp vruntime up to the floor: no head-start, no starvation (M1171) */
    }
}

/* Sleep the current task for `ms`, off-CPU, until the timer wakes it (M1079) —
 * the real, non-busy version of timer_wait(). Falls back to a HLT loop before
 * the scheduler is up. The deadline is recorded in wake_at; task_wake_sleepers()
 * (driven by the timer IRQ) flips us back to READY when it passes. */
void task_sleep_ms(uint64_t ms) {
    if (!current || current == idle_task) {       /* no scheduler / the idle floor: busy-wait */
        uint64_t target = timer_ms() + ms;
        while (timer_ms() < target) __asm__ volatile("sti; hlt");
        return;
    }
    uint64_t f = irq_save();
    current->wchan = (uint64_t)__builtin_return_address(0);   /* the sleep's caller, for WCHAN (M1166) */
    current->wake_at = timer_ms() + ms;           /* 0 ms still parks until the next tick */
    current->state = TASK_BLOCKED;
    switch_to_next();                             /* yields; woken by the timer scan */
    irq_restore(f);
}

/* Called from the timer IRQ (interrupts already off): wake every task whose
 * timed-sleep deadline has passed. The ring is tiny, so a full scan per tick is
 * cheap; only BLOCKED tasks with a non-zero wake_at are sleepers. */
void task_wake_sleepers(void) {
    if (!current) return;
    uint64_t now = timer_ms();
    task_t *t = current;
    do {
        if (t->state == TASK_BLOCKED && t->wake_at && t->wake_at <= now) {
            t->wake_at = 0;
            t->state = TASK_READY;
            t->ready_since = now;       /* re-entered the run queue (M1148) */
            sched_place_wake(t);        /* clamp vruntime up to the floor (M1171) */
        }
        t = t->next;
    } while (t != current);
}

/* ---- load average (M1148) -------------------------------------------------
 * An EWMA of the RUN-QUEUE DEPTH, using the classic Linux fixed-point scheme
 * (FSHIFT=11, FIXED_1=2048), sampled every 5 s from the timer IRQ. The sample
 * is the number of RUNNABLE tasks right now — RUNNING or READY, with the idle
 * floor excluded — i.e. how many tasks want the CPU. This replaces the old
 * hardcoded /proc/loadavg stub ("0.00 0.00 0.00") with a figure that genuinely
 * climbs under contention (spawn N spinners -> the 1-min average approaches N)
 * and decays back toward the desktop's baseline when they exit. */
#define LOAD_FSHIFT  11
#define LOAD_FIXED_1 (1u << LOAD_FSHIFT)
#define LOAD_EXP_1   1884     /* 1/exp(5s/1min)  as a FIXED_1-scaled fraction */
#define LOAD_EXP_5   2014     /* 1/exp(5s/5min)  */
#define LOAD_EXP_15  2037     /* 1/exp(5s/15min) */
static uint64_t load_avg[3];      /* fixed-point 1/5/15-min averages */
static uint64_t load_next_ms;     /* next 5 s sample boundary */

/* Tasks that want the CPU right now: RUNNING or READY, idle floor excluded.
 * Walks the (tiny) ready ring; safe from the IRQ as task_wake_sleepers does. */
int task_runnable_count(void) {
    if (!current) return 0;
    int n = 0; task_t *t = current;
    do {
        if (t != idle_task && (t->state == TASK_RUNNING || t->state == TASK_READY)) n++;
        t = t->next;
    } while (t != current);
    return n;
}

/* Update the EWMAs at most once per 5 s. Called every timer tick. */
void loadavg_sample(void) {
    uint64_t now = timer_ms();
    if (now < load_next_ms) return;
    load_next_ms = now + 5000;
    uint64_t active = (uint64_t)task_runnable_count() << LOAD_FSHIFT;
    static const uint64_t exp_[3] = { LOAD_EXP_1, LOAD_EXP_5, LOAD_EXP_15 };
    for (int i = 0; i < 3; i++)
        load_avg[i] = (load_avg[i] * exp_[i] + active * (LOAD_FIXED_1 - exp_[i])) >> LOAD_FSHIFT;
}

void task_loadavg(uint64_t out[3]) {
    out[0] = load_avg[0]; out[1] = load_avg[1]; out[2] = load_avg[2];
}

/* Charge the current task `ms` of CPU time to user or kernel mode, called from
 * the timer IRQ with user = (the interrupted frame was ring 3). Tick-sampled,
 * so coarse (one-tick granularity) — the standard getrusage utime/stime model
 * (M1150). The precise total CPU time stays in run_ms (switch_to_next). */
void task_cpu_tick(uint64_t ms, int user) {
    if (!current) return;
    if (user) current->utime_ms += ms;
    else      current->stime_ms += ms;
}

/* Set the current task's nice level (-20..+19) and its CFS weight (M1171). A
 * higher nice => smaller weight => vruntime accrues faster => less CPU. */
int task_set_nice(int nice) {
    if (!current) return 0;
    if (nice < -20) nice = -20; if (nice > 19) nice = 19;
    current->nice = nice;
    current->weight = nice_to_weight(nice);
    return nice;
}
int task_get_nice(void) { return current ? current->nice : 0; }

/* Set the current task's scheduling class (M1172). SCHED_FIFO/RR are real-time
 * (rt_priority 1..99, clamped) and preempt the SCHED_OTHER/CFS class; switching
 * back to OTHER re-places it at the CFS floor so its frozen vruntime can't make
 * it dominate or starve. Returns 0, or -1 on a bad policy. */
int task_set_sched(int policy, int rt_priority) {
    if (!current) return -1;
    if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR) return -1;
    if (policy == SCHED_OTHER) {
        current->policy = SCHED_OTHER;
        current->rt_priority = 0;
        if (current->vruntime < g_min_vruntime) current->vruntime = g_min_vruntime;
    } else {
        if (rt_priority < 1) rt_priority = 1; if (rt_priority > 99) rt_priority = 99;
        current->policy = policy;
        current->rt_priority = rt_priority;
        current->rt_ticks = RR_QUANTUM;
    }
    return 0;
}

/* Suspend a task (it leaves the run rotation until task_cont). Only a runnable,
 * non-current task — never stop ourselves (that needs a yield) or a blocked task
 * (it's already off-CPU; STOPPING it would lose the BLOCKED->READY wake path). */
void task_stop(task_t *t) {
    uint64_t f = irq_save();
    if (t && t != current && (t->state == TASK_READY || t->state == TASK_RUNNING))
        t->state = TASK_STOPPED;
    irq_restore(f);
}

/* Resume a STOPPED task. */
void task_cont(task_t *t) {
    uint64_t f = irq_save();
    if (t && t->state == TASK_STOPPED) {
        t->state = TASK_READY;
        sched_place_wake(t);            /* a long-stopped task rejoins at the floor, not dominating (M1171) */
    }
    irq_restore(f);
}

void task_exit(void) {
    irq_save();                 /* disable; this task is ending, never restore */
    task_t *dead = current;
    dead->state = TASK_DEAD;

    /* Unlink from the ring. */
    task_t *prev = dead->next;
    while (prev->next != dead)
        prev = prev->next;
    prev->next = dead->next;

    task_t *next = dead->next;
    next->state = TASK_RUNNING;
    current = next;
    next->last_in = timer_ms();   /* stamp switch-in so its CPU time isn't over-counted from a stale last_in */
    next->nswitch++;

    /* Load next's address space + kernel stack BEFORE switching, mirroring
     * switch_to_next. Otherwise the dead task's CR3 stays loaded under `next`,
     * which is unsafe the moment that CR3 is reclaimed (vmm_destroy on an app
     * exit) — `next` would be running on freed page tables. With this, an app's
     * CR3 is never the active one by the time the WM reaps it. */
    if (next->cr3 && next->cr3 != active_cr3) {
        active_cr3 = next->cr3;
        load_cr3(next->cr3);
    }
    if (next->kstack_top)
        tss_set_rsp0(next->kstack_top);
    if (next->fxbuf) fpu_restore(fxptr(next));   /* the dead task's FP state is discarded */
    load_fs_base(next->fs_base);                 /* restore the thread's TLS base (M1140) */
    context_switch(&dead->rsp, next->rsp);   /* dead->rsp save is discarded */
    /* unreachable */
}

/* Free a dead task's kernel stack and its task_t. ONLY safe on a task that has
 * exited (state==TASK_DEAD), been unlinked from the ready ring (task_exit does
 * that), and is no longer executing on its stack — i.e. reaped from a DIFFERENT
 * task's context. A task can't free its own stack (it's running on it), so a
 * separate reaper calls this once the task is off-CPU. Observing TASK_DEAD from
 * another task is sufficient proof of off-CPU: task_exit sets DEAD with
 * interrupts disabled and only leaves that region via its final context_switch,
 * so no other task can run (and observe DEAD) while this task still executes. */
void task_free(task_t *t) {
    if (!t) return;
    if (t->stack_base) kfree((void *)t->stack_base);
    if (t->fxbuf) kfree(t->fxbuf);
    kfree(t);
}

int task_current_id(void) {
    return current ? current->id : 0;        /* NULL before sched_init (matches task_snapshot/sched_tick's guard) */
}

task_t *task_self(void) {
    return current;
}

int task_count(void) {
    if (!current) return 0;                  /* ring not built yet */
    int n = 0;
    task_t *t = current;
    do { if (t->state != TASK_DEAD) n++; t = t->next; } while (t != current);
    return n;
}

/* Walk the ready ring (uninterruptibly — it's shared with the scheduler) and
 * record each task for `ps`. */
int task_snapshot(task_info_t *out, int max) {
    uint64_t f = irq_save();
    int n = 0;
    if (current) {
        task_t *t = current;
        uint64_t now = timer_ms();
        do {
            if (n >= max) break;
            out[n].id = t->id; out[n].state = (int)t->state; out[n].proc = t->proc;
            uint64_t rm = t->run_ms;
            if (t->state == TASK_RUNNING) rm += now - t->last_in;   /* include the in-progress slice */
            out[n].run_ms = rm; out[n].nswitch = t->nswitch; out[n].rq_wait_ms = t->rq_wait_ms;
            out[n].wchan = (t->state == TASK_BLOCKED) ? t->wchan : 0;   /* only meaningful while blocked (M1166) */
            out[n].nice = t->nice;                                       /* CFS nice level (M1171) */
            out[n].policy = t->policy; out[n].rt_priority = t->rt_priority;   /* scheduling class (M1172) */
            n++; t = t->next;
        } while (t != current);
    }
    irq_restore(f);
    return n;
}

/* Total ms the idle task has run — the system's idle time, for `/proc/sched`. */
uint64_t task_idle_ms(void) {
    if (!idle_task) return 0;
    uint64_t rm = idle_task->run_ms;
    if (idle_task->state == TASK_RUNNING) rm += timer_ms() - idle_task->last_in;
    return rm;
}
