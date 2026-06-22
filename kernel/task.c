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

static uint64_t active_cr3;     /* the address space currently loaded in CR3 */
static task_t *idle_task;       /* the scheduling floor: never blocks/exits, run ONLY when no other task is runnable (so a task that blocks itself when nothing else is ready hands off to this instead of spinning marked-BLOCKED). NULL until created -> switch_to_next falls back to its prior behavior. */

static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void load_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

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
    /* Prefer a non-idle runnable task: skip DEAD/BLOCKED and the idle task. */
    while (next != prev && (next->state == TASK_DEAD || next->state == TASK_BLOCKED || next == idle_task))
        next = next->next;
    if (next == prev) {                         /* no other non-idle task is runnable */
        if (prev->state == TASK_RUNNING || prev->state == TASK_READY)
            return;                             /* prev itself is still runnable (e.g. the desktop) -> keep it; idle stays parked */
        if (!idle_task || prev == idle_task)
            return;                             /* no floor available / already on it */
        next = idle_task;                       /* prev blocked/exited and nothing else is ready -> run the idle floor (don't spin a BLOCKED prev) */
    }

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current = next;

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
    current->state = TASK_BLOCKED;
    switch_to_next();
    irq_restore(f);
}

void task_wake(task_t *t) {
    if (t && t->state == TASK_BLOCKED)
        t->state = TASK_READY;
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
        do {
            if (n >= max) break;
            out[n].id = t->id; out[n].state = (int)t->state; out[n].proc = t->proc;
            n++; t = t->next;
        } while (t != current);
    }
    irq_restore(f);
    return n;
}
