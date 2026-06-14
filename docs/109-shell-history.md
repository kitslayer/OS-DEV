# Milestone 109 — shell `history` command

**Goal:** expose the command history the kernel already keeps. The line
discipline in `app_sys_read` maintains a 6-entry ring of recent commands for
up/down-arrow recall — but there was no way to *see* the list. `history` prints
it.

![the history command listing the last four commands](osdev-history.png)

## How it works

The history ring already lives in each process's `struct app`
(`hist[6][96]`, `hist_n`) and is filled whenever `app_sys_read` returns a line.
This milestone just adds a read path:

- **`SYS_history` (29)** — `(buf, len)` → `app_sys_history(buf, max)` formats the
  *calling* process's history as `"  N  command\n"` lines (oldest first,
  bounds-checked, assembled per line in a local buffer then copied only as far as
  it fits) and returns the byte count.
- **`sys_history`** in the userspace libc, and a **`history`** command in the
  shell that prints the result.

Because the history is per-process, `history` shows the shell's own recent
commands — the exact lines up/down-arrow recall walks through.

## Verified

Typed `ls`, `date`, `mem`, then `history`, which printed:

```
  1  ls
  2  date
  3  mem
  4  history
```

— the four commands, numbered oldest-first (the `history` line itself is saved
before the command runs, so it appears as entry 4). No panics. (This also
re-exercised `ls`/`date`/`mem` — FAT, RTC, and sysinfo — confirming no
regression from the milestone 104–108 core changes.)

## Files
- `kernel/include/syscall.h`, `kernel/syscall.c` — `SYS_history` dispatch
- `kernel/app.c`, `kernel/include/app.h` — `app_sys_history`
- `user/ulib.c`, `user/ulib.h` — `sys_history`
- `user/shell.c` — the `history` command + help text
