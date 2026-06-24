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
    task_t *next = prev->next;
    /* Prefer a non-idle runnable task: skip DEAD/BLOCKED/STOPPED and the idle task. */
    while (next != prev && (next->state == TASK_DEAD || next->state == TASK_BLOCKED ||
                            next->state == TASK_STOPPED || next == idle_task))
        next = next->next;
    if (next == prev) {                         /* no other non-idle task is runnable */
        if (prev->state == TASK_RUNNING || prev->state == TASK_READY)
            return;                             /* prev itself is still runnable (e.g. the desktop) -> keep it; idle stays parked */
        if (!idle_task || prev == idle_task)
            return;                             /* no floor available / already on it */
        next = idle_task;                       /* prev blocked/exited and nothing else is ready -> run the idle floor (don't spin a BLOCKED prev) */
    }

    uint64_t now = timer_ms();
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        prev->ready_since = now;                 /* entered the run queue: start clocking its wait (M1148) */
    }
    next->state = TASK_RUNNING;
    if (next->ready_since) {                      /* leaving the run queue: charge the time it waited (M1148) */
        next->rq_wait_ms += now - next->ready_since;
        next->ready_since = 0;
    }
    current = next;

    /* CPU-time accounting: credit prev with the slice it just ran, and stamp
     * next's switch-in time (for /proc/sched + a real `top`). */
    prev->run_ms += now - prev->last_in;
    next->last_in = now;
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
    if (t && t->state == TASK_STOPPED)
        t->state = TASK_READY;
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
