/*
 * timer.c — the Programmable Interval Timer (PIT), our system heartbeat.
 *
 * The PIT has a fixed input clock of 1193182 Hz. We program channel 0 with a
 * 16-bit "divisor": it counts the input clock down and raises IRQ0 each time it
 * hits zero. So output frequency = 1193182 / divisor. Pick the divisor for the
 * Hz you want.
 *
 * Why we care: a steady, periodic interrupt is the basis of timekeeping,
 * sleeping, and — crucially — *preemptive* multitasking (M7), where the timer
 * interrupt is what yanks the CPU away from a running task.
 */
#include "timer.h"
#include "interrupts.h"
#include "io.h"
#include "task.h"
#include "audio.h"
#include "profile.h"
#include "app.h"

#define PIT_CH0_DATA 0x40
#define PIT_COMMAND  0x43
#define PIT_FREQUENCY 1193182u

static volatile uint64_t ticks;
static uint32_t          tick_hz = 100;   /* IRQ0 frequency, set by timer_init */

static void timer_handler(struct registers *r) {
    ticks++;
    prof_tick(r->rip, r->cs);  /* sampling profiler: record the interrupted kernel RIP (M1086) */
    audio_pump();          /* keep the audio DMA fed (no-op unless streaming) */
    task_wake_sleepers();  /* wake any timed-sleep task whose deadline has passed (M1079) */
    app_alarm_tick();      /* raise SIGALRM if the current app's periodic alarm is due (M1102) */
    sched_tick();          /* preempt the running thread (no-op if <2 tasks) */
}

void timer_init(uint32_t hz) {
    uint32_t divisor = PIT_FREQUENCY / hz;
    tick_hz = hz;

    /* command 0x36: channel 0, access lobyte+hibyte, mode 3 (square wave). */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    irq_install_handler(0, timer_handler);   /* IRQ0, also unmasks it */
}

uint64_t timer_ticks(void) {
    return ticks;
}

/* Milliseconds since boot, derived from the tick count and the PIT frequency.
 * Monotonic; resolution is one tick (10 ms at the default 100 Hz) — coarse but
 * ample for frame pacing (e.g. DOOM's DG_GetTicksMs). */
uint64_t timer_ms(void) {
    return ticks * 1000ull / tick_hz;
}

void timer_wait(uint64_t n) {
    /* Real off-CPU sleep once the scheduler is up: the task blocks and the timer
     * IRQ wakes it at the deadline, instead of spinning READY and re-HLTing every
     * slice (which burned a slot and looked like CPU use in /proc/sched). M1079.
     * task_sleep_ms falls back to a HLT loop before the scheduler exists. */
    task_sleep_ms(n * 1000ull / tick_hz);
}
