# Milestone 9 — A userspace libc + an interactive shell

**Goal:** turn "we can run one program that prints" into "we can interact." That
means input, a small C library for user programs, and an actual shell you can
type commands into.

## Getting input into userspace

Output was easy (`SYS_write`); input needs three new pieces:

1. **A shared input queue** (`kernel/keyboard.c`). The keyboard IRQ now just
   *pushes* translated characters into a ring buffer instead of echoing. A
   blocking `input_getchar()` pulls from it, sleeping on `sti; hlt` until a
   device IRQ delivers something.
2. **Serial input too** (`kernel/serial.c`). We enable the COM1 receive
   interrupt (IRQ4); its handler pushes bytes into the *same* queue. This is
   what lets us drive the shell headlessly — piping bytes to the serial port is
   indistinguishable from typing. (Both sources, one buffer.)
3. **`SYS_read`** (`kernel/syscall.c`). Reads a line into a user buffer,
   **echoing** each character and handling **backspace** (the line-editing the
   terminal usually does for you). It blocks until Enter.

## A real libc, in miniature (`user/ulib.c`)

`ulib` is the seed of a C library that links into every user program:
- **`_start`** — the ELF entry point. It calls `main()` and passes the return
  value to `SYS_exit`. (This is what a libc's `crt0` does.)
- **syscall wrappers** — `sys_write`, `sys_read`, `sys_exit`, `sys_getpid`.
- **helpers** — `print`, `readline`, `streq`, `startswith`, `ustrlen`.

## The shell (`user/shell.c`)

A classic read-eval loop, entirely in ring 3: print a prompt, `readline`, match
built-ins (`help`, `echo`, `ver`, `pid`, `exit`). Every action is a syscall —
this is a genuine userspace program using our OS the way `sh` uses Linux.

## What we proved
Piping `help / echo … / ver / pid / exit` over the serial line produced correct
output for each, including unknown-command handling, and `exit` returned cleanly
to the kernel. (One cosmetic note: dumping *all* input at boot can race the
serial-RX setup and drop the very first byte — real typing, which arrives after
boot, doesn't hit this.)

## Files
- `kernel/keyboard.c` — input ring buffer + blocking reader
- `kernel/serial.c` — serial RX interrupt feeding the same buffer
- `kernel/syscall.c` — `SYS_read` line editing
- `user/ulib.c`, `user/ulib.h` — the userspace runtime/libc
- `user/shell.c` — the shell
