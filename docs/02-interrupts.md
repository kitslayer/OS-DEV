# Milestone 2 — Interrupts (GDT/TSS, IDT, exceptions, PIC)

**Goal:** let the CPU call *our* code when something happens — a fault, or a
hardware signal. This is the foundation of every interactive and preemptive
feature later.

## 1. The GDT, even in long mode (`kernel/gdt.c`, `kernel/asm/gdt_flush.asm`)

Long mode mostly disables segmentation (base 0, no limits), but a few things
are still read from segment **descriptors**: the privilege ring (0 = kernel,
3 = user), the "L" bit marking a code segment as 64-bit, and the TSS location.

So we build: null, kernel code/data, user code/data (for later), and a TSS.

**The far-return trick:** `lgdt` only points the CPU at the table; the segment
registers still cache the old descriptors. Data segments refresh with `mov`,
but **`CS` can only change via a control transfer**. In long mode we push the
new `CS` + a return address and execute `retfq`, which pops both at once. That's
the only reason `gdt_flush` is assembly.

## 2. The TSS: stacks for trouble

In long mode the TSS holds stacks, not registers:
- **`RSP0`**: the stack the CPU switches to on a ring 3 → ring 0 interrupt
  (needed once we have userspace — a user stack can't be trusted).
- **The IST**: up to 7 guaranteed-good stacks an interrupt gate can demand. We
  run the **double fault** on IST1, so a fault caused by a trashed kernel stack
  can still be reported instead of escalating to a triple fault (reboot).

## 3. The IDT: the CPU's jump table (`kernel/idt.c`)

256 sixteen-byte **gate descriptors**. On vector *N*, the CPU reads entry *N*,
optionally switches to an IST stack, and jumps to the handler address (stored
split across three fields). We fill 0–47; the rest stay "not present." Gate
type `0x8E` = present, ring 0, interrupt gate (auto-clears IF on entry).

## 4. The assembly stubs: uniform frames (`kernel/asm/isr_stubs.asm`)

The hardware is inconsistent — some exceptions push an **error code**, most
don't. Each stub pushes a **dummy 0** (or relies on the real one) plus the
**vector number**, so every entry has the same shape. `isr_common` then pushes
all 15 GP registers and calls `isr_dispatch(rsp)`. That `rsp` *is* a
`struct registers` — so the struct's field order must mirror the push order
exactly. `iretq` reverses it all and resumes.

## 5. The PIC remap: avoiding a fatal collision (`kernel/pic.c`)

The legacy 8259 PIC defaults to delivering IRQ0–7 as CPU vectors **8–15** — but
vector 8 is `#DF`, the double fault! A keystroke would look like a crash. We
reprogram the PICs (the ICW1–4 sequence) to use vectors **0x20–0x2F**, clear of
the exception range, then **mask all IRQs**. Drivers unmask their own line when
ready. After handling an IRQ we must send an **EOI** or the PIC goes silent.

## 6. The dispatcher (`kernel/interrupts.c`)

- vectors 0–31: a CPU **exception**. Breakpoint (`#BP`) is reported and resumed;
  anything else prints a full register dump + decoded info and halts. For page
  faults we also read **`CR2`** (the faulting address) — the exact mechanism M5
  will build demand paging on.
- vectors 32–47: a hardware **IRQ**. Call the registered handler, send EOI.

## What we proved
- `int3` round-tripped through the IDT and came back via `iretq`.
- A deliberate write to unmapped memory produced a clean page-fault panic with
  `error_code=0x2` (not-present + write + kernel) and the right `CR2`.

## Files
- `kernel/gdt.c`, `kernel/asm/gdt_flush.asm`
- `kernel/idt.c`, `kernel/asm/isr_stubs.asm`
- `kernel/pic.c`, `kernel/interrupts.c`
- `kernel/lib/string.c` (memset/memcpy — the compiler can emit these)
