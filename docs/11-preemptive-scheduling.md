# Milestone 11 — Preemptive scheduling

**Goal:** stop trusting threads to cooperate. The kernel should be able to take
the CPU away from a thread that never calls `task_yield()` — which is what
"multitasking" really means.

## The idea

We already had a context switch (M7) and a periodic timer interrupt (M3). To
preempt, we simply **switch tasks from inside the timer IRQ**: every tick, the
timer handler calls `sched_tick()`, which (if more than one task exists) does a
normal context switch to the next task. The interrupted thread's full state is
already saved on its kernel stack by the interrupt stub, so it resumes exactly
where it was when its turn comes around again.

## The one subtlety: acknowledge the interrupt *first*

The PIC won't deliver another IRQ of the same line until it gets an
**end-of-interrupt (EOI)**. Normally we send the EOI *after* the handler runs.
But a preempting timer handler **context-switches away and doesn't return** —
not until the preempted task is scheduled again. If the EOI is still pending at
that point, the PIC goes silent and preemption stalls after one switch.

The fix (`kernel/interrupts.c`): send the EOI **before** calling the IRQ
handler. This is safe because interrupt gates keep interrupts disabled during
the handler, so there's no re-entry to worry about.

## Why it's safe to switch inside an interrupt

- The timer fires while a thread runs in ring 0; same privilege means **no stack
  switch**, so the interrupt frame lands on that thread's own kernel stack.
- `sched_tick()` runs with interrupts already disabled (interrupt gate), so the
  ready-ring isn't mutated underneath it. The cooperative paths
  (`task_create`/`task_exit`/`task_yield`) also disable interrupts while they
  touch the ring, so the two never collide.
- When the preempted thread is resumed, the context switch returns up through
  the timer handler and the stub, which restores every register and `iretq`s
  back to the exact instruction that was interrupted.

## What we proved

We spawned a worker stuck in `while (!stop) spin_count++;` — it never yields.
Under cooperative scheduling that would freeze `main` the moment the worker got
the CPU. With preemption, `main` kept printing every ~0.4s **and** the worker's
counter raced past 491 million. Both ran; neither cooperated. That's preemption.

## Files
- `kernel/timer.c` — the tick handler now calls `sched_tick()`
- `kernel/task.c` — `sched_tick()` (preempt if >1 task)
- `kernel/interrupts.c` — EOI moved before the handler

## What this unlocks / what's still missing
Threads now share the CPU fairly without cooperating. Still to come (see
WHATS-NEXT.md): a real **time quantum** (switch every N ms instead of every
tick), thread **priorities/sleep queues** (so idle threads `hlt` instead of
busy-looping), and — the big one — **per-process address-space isolation** so
preemptable *user* processes can't see each other's memory.
