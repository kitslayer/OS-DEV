# Milestone 3 — Timer + keyboard

**Goal:** make the kernel react to the outside world. Two hardware IRQs go
live: a periodic timer (IRQ0) and the keyboard (IRQ1). This is the first payoff
of all the interrupt plumbing from M2.

## The PIT — a heartbeat (`kernel/timer.c`)

The Programmable Interval Timer has a fixed 1193182 Hz input clock. You load
channel 0 with a 16-bit **divisor**; it counts the clock down and raises
**IRQ0** each time it reaches zero. So:

```
output frequency = 1193182 / divisor
```

For 100 Hz we write divisor `11932` (low byte then high byte to port 0x40,
after a mode command to 0x43). Our handler does almost nothing — just `ticks++`
— but that counter is the seed of three things:

- **timekeeping** (`timer_ticks()`),
- **sleeping** (`timer_wait()` halts until enough ticks pass),
- **preemptive multitasking** (M7): the timer interrupt is precisely the moment
  we'll forcibly switch tasks.

We proved it by sleeping ~1s and watching the count go `1 → 101`.

## The keyboard (`kernel/keyboard.c`)

The keyboard controller raises **IRQ1** on every key state change and leaves a
**scan code** in port `0x60`. In scan-code set 1 (the default):

- a **press** is the code itself (e.g. `0x1E` for `A`),
- a **release** is the same code OR'd with `0x80`.

The driver:
1. reads the scan code,
2. if it's a release, only updates Shift state and returns,
3. otherwise looks the code up in a US-QWERTY table (a second table for when
   Shift is held) and echoes the character.

This is a polling-from-the-IRQ design: the IRQ tells us "a byte is ready," and
we read exactly one byte. (Test it interactively with `make run`.)

## The pattern to notice

Both drivers do the same three-step dance, which every device driver from here
follows:
1. **configure** the device through its I/O ports,
2. **register** an IRQ handler with `irq_install_handler()` (which also unmasks
   that line on the PIC),
3. the handler **services** the device and returns; the dispatcher sends the
   EOI.

## Files
- `kernel/timer.c` — PIT, tick counter, sleep
- `kernel/keyboard.c` — PS/2 scan-code translation + echo
