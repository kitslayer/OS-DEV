# Milestone 23 — Sleep/wake (proper blocking)

**Goal:** fix the M22 limitation where an app waiting for input **busy-yielded**
(spinning the CPU at 100%). Now a waiting task truly **sleeps** and is woken
when input arrives, so the kernel can idle (`hlt`).

## The mechanism (`kernel/task.c`)

- A new task state, **`TASK_BLOCKED`**. The scheduler's run-rotation skips
  blocked (and dead) tasks, so a blocked task simply isn't given CPU time.
- **`task_block()`**: mark the current task blocked and switch away. It runs
  with interrupts off across the state change + switch so a wakeup can't be
  lost.
- **`task_wake(t)`**: mark a blocked task runnable again; the scheduler will run
  it on the next switch.

## Using it (`kernel/app.c`)

`SYS_read` used to `task_yield()` in a loop while the input queue was empty —
correct, but it spun. Now it calls **`task_block()`**: the app sleeps. When the
window manager delivers a keystroke to the focused app (`app_key`), it calls
**`task_wake()`** on that app's task. The app wakes, re-checks its queue, and
reads the input.

## Why it's race-free

The block runs with interrupts disabled, and on a single CPU the only thing that
delivers input (the WM, via `app_key`) runs as a *different* task — so the app
always sets itself `BLOCKED` *before* yielding the CPU to whoever might wake it.
No lost wakeups.

## Result

When all apps are waiting for input, no task is runnable except the window
manager, which composites and then `hlt`s — so the **CPU idles** instead of
spinning. The shell still works exactly as before; it just sleeps at its prompt
now (visible in the screenshot: the shell is blocked at `osdev$`, consuming no
CPU).

## Files
- `kernel/task.c` — `TASK_BLOCKED`, `task_block`, `task_wake`, scheduler skip
- `kernel/app.c` — `SYS_read` blocks; `app_key` wakes
