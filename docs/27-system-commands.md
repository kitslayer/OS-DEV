# Milestone 27 — System commands (mem / clear / reboot)

**Goal:** let userspace query and control the system — memory/uptime stats,
clearing the terminal, and rebooting.

## New syscalls (`kernel/syscall.c`)
- **`SYS_sysinfo`** — formats a summary into a user buffer: RAM free/total (from
  the PMM), uptime (from the timer), and the live task count (`task_count()`).
- **`SYS_clear`** — clears the calling app's terminal grid.
- **`SYS_reboot`** — pulses the 8042 keyboard-controller reset line
  (`out 0x64, 0xFE`) to reboot the machine.

## New shell commands
`mem` (sysinfo), `clear`, and `reboot`. Verified by typing `mem` into the shell:

```
osdev$ mem
RAM:    123 MiB free / 127 MiB
uptime: 5 s
tasks:  2
```

(Two tasks = the window manager + the shell process.)

## Files
- `kernel/syscall.c` — the three syscalls + small string-builder helpers
- `kernel/task.c` — `task_count()`
- `kernel/app.c` — `app_sys_clear()`
- `user/shell.c`, `user/ulib.c` — `mem` / `clear` / `reboot`
