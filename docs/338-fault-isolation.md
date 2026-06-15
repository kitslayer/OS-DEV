# 338: ring-3 fault isolation — one app can no longer crash the OS

Until this point, **any** CPU exception took down the whole kernel. `isr_dispatch`
(kernel/interrupts.c) handled every vector `< 32` (except the recoverable
breakpoint `#3`) by printing a panic banner and halting forever:

```c
interrupts_disable();
kprintf("*** KERNEL PANIC: CPU EXCEPTION ***\n");
...
for (;;) __asm__ volatile("cli; hlt");
```

That's correct for a **kernel** bug — but the same path fired for a *userspace*
fault. So a single buggy ring-3 app — a null dereference, a stack overflow, a
divide error — froze the entire desktop. A periodic review (see the M308–337
app suite) found a concrete trigger: the calculator could compute
`LONG_MIN / -1` (typeable as `0x8000000000000000 / 0xffffffffffffffff`), which
raises a divide-error (#DE) on x86 — and that halted the OS.

## The fix: terminate the task, not the kernel

A fault's saved `CS` carries the privilege level of the faulting code in its low
two bits. Ring-3 user code runs with `CS` RPL 3; the kernel runs RPL 0. So
`isr_dispatch` now branches on that:

```c
if ((r->cs & 3) == 3) {            /* fault happened in a ring-3 (app) task */
    kprintf("[fault] %s (vector %lu) ... in a ring-3 task -- terminating it\n", ...);
    app_fault_current();           /* does not return */
}
/* a ring-0 fault falls through and still panics — that IS a real kernel bug */
```

`app_fault_current()` (kernel/app.c) mirrors the proven `app_sys_exit()` exit
path — it marks the faulting task's app exited (so the window manager tears down
the window) and calls `task_exit()`, which unlinks the task from the round-robin
ring and `context_switch`es to the next task. The dead task never resumes; its
abandoned exception frame (and any CPU-pushed error code) is simply discarded.

This is safe for the same reason preemption is: the **timer IRQ already
context-switches from inside an interrupt**, so switching away from within the
fault ISR is an established pattern, not a new one.

## Result

A misbehaving app now dies alone: the kernel logs `[fault] … terminating it`,
the app's window and taskbar entry disappear, and the rest of the desktop keeps
running (verified live — a deliberate `calc` divide error killed only `calc`;
`F2` still cycled windows afterward). Real kernel faults (ring 0) still panic, so
genuine kernel bugs are still loud. The calculator additionally guards the
`LONG_MIN / -1` (and `%`) overflow so it reports a syntax error rather than
faulting at all — defence in depth on top of the kernel's new safety net.

## Files

- `kernel/interrupts.c` — the `CS & 3 == 3` branch in `isr_dispatch`.
- `kernel/app.c` / `kernel/include/app.h` — `app_fault_current()`.
- `user/calc.c` — the overflow guard (the immediate trigger).
