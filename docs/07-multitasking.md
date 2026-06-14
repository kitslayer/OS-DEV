# Milestone 7 — Multitasking + scheduler

**Goal:** run more than one thread of execution and switch between them. This is
the conceptual leap from "a program" to "an operating system."

## The core trick: a thread *is* its stack

Everything a running function is doing — local variables, where it is, what it
calls next — lives on its stack and in registers. So to switch threads you only
need to **save the current stack pointer and load another**. That's
`context_switch` (`kernel/asm/context_switch.asm`):

1. push the **callee-saved** registers (the C ABI says the caller already saved
   the rest around the call),
2. `*old_rsp = rsp` — remember where we were,
3. `rsp = new_rsp` — adopt the other thread's stack,
4. pop those registers back and `ret`.

That `ret` is the magic: it resumes whatever the new thread was doing when *it*
last called `context_switch`. The CPU doesn't know threads exist — we're just
swapping stacks under it.

## Bootstrapping a brand-new thread (`task_create`)

A new thread has never run, so there's nothing to "resume." We fake it: we
hand-build an initial stack that looks exactly like one `context_switch` left
behind — six zeroed registers and a **return address pointing at a trampoline**.
The first switch-in pops the zeros and `ret`s into `thread_trampoline`, which
enables interrupts and calls the thread's entry function.

## The scheduler (`kernel/task.c`)

Tasks live in a **circular linked list** (the "ready ring"); `current` points at
the running one. Scheduling is just "switch to `current->next`" — that's
**round-robin**. `task_exit` unlinks a finished thread from the ring and
switches away, never returning.

Because scheduling edits a shared list, it runs with interrupts disabled
(`irq_save`/`irq_restore`), and we restore the caller's interrupt state
afterward — which matters for the cooperative-vs-preemptive distinction below.

## Cooperative now, preemptive later

Ours is **cooperative**: a thread runs until it calls `task_yield()`. The demo
spawns three workers and you can see the scheduler round-robin between them:
`C1 B1 A1 → C2 B2 A2 → …`. We kept it cooperative so the output is deterministic.

**Preemptive** scheduling — forcibly switching a thread that never yields — is a
small addition: have the **timer IRQ** (M3) call `schedule()`. The one catch is
interrupt acknowledgement: the timer handler must send its **EOI before**
switching away, or the PIC won't deliver the next tick to the thread we switch
to, and preemption stalls. The plumbing (timer + IRQ dispatch) is already there.

## What we proved
Three threads interleaved cleanly in round-robin order, each ran to completion,
exited, and control returned to the original task. Context save/restore, stack
bootstrapping, and ring management all work.

## Files
- `kernel/asm/context_switch.asm` — save/restore + stack swap
- `kernel/task.c` — task structs, ready ring, scheduler, create/yield/exit
