# Milestone 21 — Per-process address spaces (memory isolation)

**Goal:** give each process its own view of memory, so programs can't read or
corrupt each other. This is the keystone for running real, separate userspace
apps — and the security boundary every multitasking OS depends on.

## The idea

Until now every task shared one set of page tables. Now each process gets its
own **PML4** (top-level page table), loaded into **CR3** when the scheduler
switches to it. Two processes can use the *same* virtual address for completely
different physical memory.

## Sharing the kernel, isolating the user (`kernel/vmm.c`)

The trick is what a new address space *shares* vs what it keeps *private*. The
kernel must be reachable in every address space (so syscalls and interrupts work
no matter who's running). So `vmm_create_address_space()`:

- **copies the entire higher half** (PML4 entries 256–511) from the kernel's
  table → the HHDM and kernel heap are shared;
- gives the low half its **own PDPT**, into which it copies the kernel's
  identity + device-MMIO entries → kernel code, data, stacks, and the
  framebuffer are shared, but the rest of the low region is blank and private.

So user mappings added at, say, `0x40000000` live in that process's private PDPT
and are invisible to every other process.

Deliberately, this **avoids relocating the kernel to the higher half** (which
would mean rewriting the fragile early boot assembly). Sharing at the PDPT level
gets the same isolation with far less risk.

## Switching address spaces (`kernel/task.c`)

Each `task` gains a `cr3`. The scheduler, right before the context switch, loads
the next task's CR3 if it differs from the one currently active. This is safe
*because* the kernel is mapped everywhere: the code doing the switch, the kernel
stack it's running on (in the shared heap), and the GDT/IDT/TSS are all valid
before and after the CR3 change.

## What we proved

Three processes each mapped a private page at the **identical** address
`0x40000000` and wrote their own id. Across interleaved scheduling, each one
*always* read back its own value:

```
[proc 2] *0x40000000 = 200  isolated
[proc 3] *0x40000000 = 300  isolated
[proc 4] *0x40000000 = 400  isolated   (repeated, never a "LEAK!")
```

A shared address space would have them clobbering each other; separate page
tables keep them apart. And the desktop still boots — proof the shared kernel
mappings are correct.

## What this unlocks

This is the foundation for **`fork`/`exec`**, running multiple *userspace*
programs at once, and eventually hosting real apps as windows — each in its own
protected address space.

## Files
- `kernel/vmm.c` — `vmm_create_address_space`, `vmm_map_to`
- `kernel/task.c` — per-task `cr3`, CR3 switch in the scheduler
