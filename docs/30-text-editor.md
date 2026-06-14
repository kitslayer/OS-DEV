# Milestone 30 — A userspace text editor

**Goal:** an actual content-creation app — type a document and save it to disk.

## `edit <file>` (`user/shell.c`)

A tiny line editor, built **entirely from existing syscalls** (no new kernel
code): it prints a `>` prompt, reads lines with `SYS_read`, and accumulates them
until you enter a line that is just `.`, then saves the whole buffer to the
FAT32 disk with `SYS_writefile` (M28).

That it needed zero kernel changes is the point: by this stage the syscall
surface (read / write / writefile) is rich enough that real userspace programs
compose cleanly on top of it.

## What we proved
```
osdev$ edit poem.txt
> roses are red
> violets blue
> .
saved poem.txt
osdev$ cat poem.txt
roses are red
violets blue
```
The file showed up in the live Files listing as `POEM.TXT (27b)` and read back
correctly — a full create → save → list → read cycle.

## Files
- `user/shell.c` — the `edit` command (on top of `SYS_read`/`SYS_writefile`)
