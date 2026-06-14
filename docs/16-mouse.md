# Milestone 16 — PS/2 mouse + cursor

**Goal:** a moving pointer. A desktop is unusable without one.

## The hardware (`kernel/mouse.c`)

The mouse shares the **8042 PS/2 controller** with the keyboard but lives on the
*second* port and raises **IRQ12**. Two wrinkles:

- IRQ12 is on the **slave PIC**, so its interrupts only reach the CPU if the
  **cascade line (IRQ2)** on the master is also unmasked.
- You enable the second port and turn on its IRQ by reading/modifying the
  controller's **config byte** (commands `0x20`/`0x60`), then tell the mouse to
  use defaults (`0xF6`) and start reporting (`0xF4`), each answered by an ACK.

The mouse then streams **3-byte packets**: a flags byte (button states + the
sign bits for the deltas) followed by signed X and Y movement. The IRQ12 handler
assembles packets with a small state machine (re-syncing on the always-set bit 3
of byte 0), accumulates an absolute position clamped to the screen, and records
the buttons.

## Robustness: never spin forever

The first attempt **hung at boot** — the controller handshake busy-waited on a
status bit that never changed under QEMU. The fix: every wait is now
**bounded** (give up after a spin count), the output buffer is **flushed**
before init, and the ACK read is optional. A driver should degrade, not deadlock.

## The cursor

Software cursor: a 12×19 arrow bitmap (black outline, white fill, transparent
elsewhere). `mouse_render` **saves the pixels under the cursor**, draws the
arrow, and restores them before moving — so it floats over whatever's on screen
without corrupting it. (The compositor in M17 redraws every frame instead, so
it won't need save-under.)

## What we proved

Injecting mouse movement via QEMU's monitor moved the on-screen arrow from the
center toward the bottom-right, with the console text behind it untouched — the
packets are decoded and the position tracks correctly.

## Files
- `kernel/mouse.c`, `kernel/include/mouse.h` — driver, packet decode, cursor
- builds on the PIC (IRQ2 cascade + IRQ12) and the framebuffer
