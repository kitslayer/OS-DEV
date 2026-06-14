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

#define STACK_SIZE 16384

static task_t *current;
static int     next_id;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

static uint64_t active_cr3;     /* the address space currently loaded in CR3 */

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

void sched_init(void) {
    task_t *t = kzalloc(sizeof(task_t));
    t->id = next_id++;
    t->state = TASK_RUNNING;
    t->next = t;                /* a ring of one */
    t->cr3 = read_cr3();        /* task 0 runs in the kernel's address space */
    active_cr3 = t->cr3;
    current = t;
}

task_t *task_create(void (*entry)(void), uint64_t cr3, void *proc) {
    return task_create_stack(entry, cr3, proc, STACK_SIZE);
}

/* Like task_create but with an explicit kernel-stack size — for tasks that run
 * deep/heavy code (e.g. the browser fetch worker, which does bignum/RSA crypto
 * that would overflow the default 16 KB stack). */
task_t *task_create_stack(void (*entry)(void), uint64_t cr3, void *proc, int stack_size) {
    task_t *t = kzalloc(sizeof(task_t));
    t->id = next_id++;
    t->entry = entry;
    t->state = TASK_READY;
    t->cr3 = cr3;             /* set BEFORE the ring insert: no startup race */
    t->proc = proc;

    uint8_t *stack = kmalloc(stack_size);
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
    while (next != prev && (next->state == TASK_DEAD || next->state == TASK_BLOCKED))
        next = next->next;
    if (next == prev)
        return;                 /* nothing else runnable */

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
    context_switch(&dead->rsp, next->rsp);   /* dead->rsp save is discarded */
    /* unreachable */
}

int task_current_id(void) {
    return current->id;
}

task_t *task_self(void) {
    return current;
}

int task_count(void) {
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
