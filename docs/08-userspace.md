# Milestone 8 — Userspace, syscalls, and an ELF loader

**Goal:** run a real, separately-compiled program in **ring 3** (unprivileged),
able to reach the kernel only through **system calls**. This is the
kernel/application boundary that defines an operating system.

## The three pieces

### 1. An ELF loader (`kernel/elf.c`)
A program on disk is an ELF file: a header (entry point + where the program
headers are) followed by segments. Each `PT_LOAD` segment says "put these
`filesz` bytes at this virtual address, and zero-fill up to `memsz`" (the extra
is `.bss`). The loader allocates frames, maps them as **user** pages, and
copies. It returns the entry point.

Since we have no filesystem yet, the program (`user/init.c`) is compiled to its
own ELF and **embedded** in the kernel via `incbin` (`kernel/asm/user_blob.asm`).

### 2. Entering ring 3 (`kernel/asm/usermode.asm`)
There is no "jump to ring 3" instruction — you *return* into it. We build a fake
interrupt-return frame (`SS, RSP, RFLAGS, CS, RIP`) using the **user** code/data
selectors and execute `iretq`. The CPU pops it and lands in ring 3 on the user
stack. We first stash the kernel's stack pointer so `SYS_exit` can jump back
(`return_to_kernel`) — a one-way longjmp out of userspace.

### 3. System calls (`kernel/syscall.c`, `kernel/include/syscall.h`)
Ring 3 can't call kernel functions directly. The user puts a call number in
`rax` + args in `rdi/rsi/rdx` and runs `int 0x80`. We add an IDT gate for vector
0x80 **with DPL 3** (so ring 3 is even allowed to invoke it — otherwise it would
`#GP`). The existing interrupt stub saves the user's registers; the dispatcher
reads them, does the work, and writes the result back into the saved `rax`,
which is restored on `iretq`. We implemented `write`, `getpid`, and `exit`.

## The bug worth remembering

The first run page-faulted at the entry point with `error_code=0x5` — *page
present, user access denied*. The `USER` permission bit must be set at **every
level** of the page-table walk (PML4 → PDPT → PD → PT), and `boot.asm`'s
top-level entry was supervisor-only. The VMM only set `USER` when *creating* a
table, not when a user mapping passed through an *existing* one. The fix
(`vmm.c next_table`) upgrades existing intermediate entries — safe, because
kernel leaf pages still lack `USER`, and the effective permission is the AND
across all levels.

## A deliberate simplification: one address space

We run the user program in the **same** page tables as the kernel (the kernel's
low identity map is supervisor-only, so ring 3 can't touch it). This gives real
privilege separation but **not** per-process memory isolation — two user
programs would share one address space at different addresses. Full isolation
needs a separate set of page tables per process (a new PML4 per process, with
the kernel mapped in the shared higher half), which in turn wants the
higher-half kernel relocation noted in docs/05. A good next step beyond M10.

## Files
- `kernel/elf.c` — ELF64 parser/loader
- `kernel/asm/usermode.asm` — `enter_user` / `return_to_kernel`
- `kernel/syscall.c` — syscall dispatcher
- `user/init.c`, `user/user.ld` — the embedded ring-3 program
