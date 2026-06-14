# Milestone 0 — Booting to 64-bit long mode

**Goal:** go from "QEMU powers on" to "our C function `kmain()` is running in
64-bit mode," and prove it by printing text.

## The problem

A PC doesn't boot into 64-bit mode. The firmware/bootloader hands control to
our kernel in **32-bit protected mode with paging disabled**. We have to climb
the rest of the way to **64-bit long mode** ourselves, in assembly, before any
C can run.

We also chose to be loaded by QEMU's built-in **Multiboot** loader (`-kernel`),
which means we don't write a bootloader or build an ISO at all.

## The pieces

### The Multiboot header (`boot/boot.asm`)
The loader scans the start of our binary for a 3-word magic header
(`0x1BADB002`, flags, checksum). Finding it, it loads the kernel at physical
1 MiB and jumps to our `_start` in 32-bit mode.

### The 32 → 64-bit trampoline (`boot/boot.asm`)
In 32-bit code we:
1. **verify** we were booted by a multiboot loader (magic `0x2BADB002` in eax),
2. use **CPUID** to confirm the CPU actually supports long mode,
3. build **page tables** (long mode requires paging to even be on) that
   identity-map the low 1 GiB using 2 MiB pages — virtual address == physical,
4. enable **PAE** (CR4), set **LME** (the EFER MSR), and turn on **paging**
   (CR0.PG). That combination *is* "enter long mode,"
5. load a 64-bit **GDT** and **far-jump** to a 64-bit code segment.

### The 64-bit entry (`boot/boot.asm` → `kmain` in `kernel/kmain.c`)
Reload the data segment registers, set up a stack, and `call kmain`.

## The one real gotcha: ELF64 vs `qemu -kernel`

QEMU's multiboot loader **refuses to load a 64-bit ELF** ("give a 32bit one").
The fix (`Makefile`): link the kernel as a normal **ELF64** (so all our C is
real 64-bit code, with symbols for `gdb`), then `objcopy` it into a **32-bit
ELF container** that the loader accepts. The 64-bit machine code inside is
untouched — only the ELF *header* changes.

## How we proved it

`kmain()` writes to the VGA text buffer at `0xB8000` **and** the COM1 serial
port. `make test` boots headless and captures the serial output, so we can
confirm the kernel reached C without even opening a window.

## Files
- `boot/boot.asm` — multiboot header + trampoline
- `linker.ld` — load at 1 MiB, multiboot header first
- `Makefile` — build, the ELF64→ELF32 `objcopy`, run/test
- `kernel/kmain.c` — first C
