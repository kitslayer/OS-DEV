/* task.h — kernel threads + round-robin scheduler. */
#pragma once
#include <stdint.h>

typedef enum { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_DEAD, TASK_STOPPED } task_state_t;

struct registers;   /* (interrupts.h) — the trap frame, captured for /proc/<pid>/regs */

typedef struct task {
    uint64_t      rsp;        /* saved stack pointer — MUST be the first field */
    struct task  *next;       /* circular ready-ring link */
    void        (*entry)(void);
    uint64_t      stack_base;
    uint64_t      kstack_top;  /* TSS rsp0 for this task (ring3->ring0 traps)  */
    uint64_t      cr3;         /* address space (0 = use the kernel's)         */
    void         *proc;        /* opaque per-task data (userspace app, if any) */
    int           id;
    task_state_t  state;
    uint8_t      *fxbuf;       /* FXSAVE area (512B + slack, aligned to 16 at use) */
    uint64_t      run_ms;      /* total ms this task has been RUNNING (CPU time)   */
    uint64_t      last_in;     /* timer_ms() when it last became `current`         */
    uint64_t      nswitch;     /* times it has been scheduled in (context switches) */
    uint64_t      utime_ms;    /* CPU time charged in user mode (ring 3), tick-sampled (getrusage, M1150) */
    uint64_t      stime_ms;    /* CPU time charged in kernel mode (ring 0), tick-sampled (M1150) */
    uint64_t      nvcsw;       /* voluntary context switches: it blocked/yielded (M1150) */
    uint64_t      nivcsw;      /* involuntary context switches: it was preempted (M1150) */
    uint64_t      rq_wait_ms;  /* total ms spent READY-but-not-running (run-queue wait, /proc/sched) (M1148) */
    uint64_t      ready_since; /* timer_ms() when it last entered the run queue; 0 = not waiting (M1148) */
    uint64_t      wchan;       /* kernel PC where it last blocked (task_block/sleep), for /proc/sched WCHAN (M1166) */
    uint64_t      wake_at;     /* if BLOCKED via task_sleep_ms: timer_ms() deadline (0 = not a timed sleep) */
    struct registers *uframe;  /* most recent ring-3 trap frame (for /proc/<pid>/regs); valid while stopped (M1119) */
    struct registers *start_frame;  /* a thread's initial ring-3 frame: iret'd to once at startup, then freed (M1138) */
    uint64_t      fs_base;     /* per-thread %fs base for TLS; 0 = unused (restored on switch, M1140) */
    uint64_t      robust;      /* userspace robust_t* (held robust locks); walked on exit (M1141) */
} task_t;

void    sched_init(void);                  /* adopt the current context as task 0 */
/* Spawn a task. cr3 = address space (0 for the kernel's); proc = opaque data.
 * Both are set before the task can be scheduled (no startup race). */
task_t *task_create(void (*entry)(void), uint64_t cr3, void *proc);
task_t *task_create_stack(void (*entry)(void), uint64_t cr3, void *proc, int stack_size);
void    schedule(void);                    /* switch to the next ready task */
void    task_yield(void);                  /* voluntarily give up the CPU */
void    task_exit(void);                   /* end the current thread (no return) */
void    task_free(task_t *t);              /* free a DEAD, unlinked, off-CPU task (reaper only) */
int     task_current_id(void);
task_t *task_self(void);                   /* the currently running task */
void    task_set_fs_base(uint64_t b);      /* set the current thread's %fs (TLS) base (M1140) */
void    task_set_robust(uint64_t r);       /* register the current thread's robust-futex list (M1141) */
uint64_t task_robust(void);                /* the current thread's robust-list ptr (0 = none) (M1141) */
void    task_block(void);                  /* block current task until woken */
void    task_wake(task_t *t);              /* mark a blocked task runnable again */
void    task_sleep_ms(uint64_t ms);        /* sleep the current task off-CPU until the timer wakes it */
void    task_wake_sleepers(void);          /* timer IRQ: wake tasks whose sleep deadline passed */
void    task_copy_fpu(task_t *dst, task_t *src);   /* clone src's live FP/SSE state into dst (fork) */
struct registers *task_uframe(task_t *t);  /* the task's most recent ring-3 trap frame, or 0 (M1119) */
void    task_stop(task_t *t);              /* suspend another task (READY/RUNNING -> STOPPED); not self */
void    task_cont(task_t *t);              /* resume a STOPPED task */
int     task_count(void);                  /* number of live tasks */

/* A snapshot of one task, for `ps` and `/proc/sched`. */
typedef struct { int id; int state; void *proc; uint64_t run_ms; uint64_t nswitch; uint64_t rq_wait_ms; uint64_t wchan; } task_info_t;
int     task_snapshot(task_info_t *out, int max);   /* fill out[]; returns count */
uint64_t task_idle_ms(void);                         /* ms the idle task has run (system idle time) */
int     task_runnable_count(void);   /* tasks wanting the CPU now (RUNNING|READY, idle excluded) (M1148) */
void    loadavg_sample(void);        /* called each timer tick; updates the EWMA once per 5 s (M1148) */
void    task_loadavg(uint64_t out[3]); /* fixed-point (FSHIFT=11) 1/5/15-min load averages (M1148) */
void    task_cpu_tick(uint64_t ms, int user);  /* timer: charge current task ms of user/kernel CPU time (M1150) */

/* Called from the timer IRQ to preempt the running thread (no-op until the
 * scheduler is initialized and there's more than one task). */
void    sched_tick(void);
