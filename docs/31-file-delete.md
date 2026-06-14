# Milestone 31 — File delete (`rm`)

**Goal:** complete file management — alongside create/read/write/edit, the
ability to delete.

## `fat32_delete` (`kernel/fat32.c`)

Deleting a FAT file is two steps:
1. **Free its cluster chain** — walk the chain from the directory entry's first
   cluster, setting each FAT entry back to 0 (free).
2. **Mark the directory entry deleted** — set the first name byte to `0xE5`
   (the FAT "deleted" marker) and write the directory sector back.

Exposed via a `vfs_remove` op, a `SYS_delete` syscall, and a shell **`rm`**
command.

## What we proved
```
osdev$ rm note.txt
removed note.txt
osdev$ ls
README.TXT 90
HELLO.TXT 42
MOTD.TXT 78
POEM.TXT 27
```
`NOTE.TXT` is gone from both the shell listing and the live Files window — the
deletion persisted to the disk.

## The file toolkit is now complete
`ls` · `cat` · `edit` · `write` · `rm` — list, read, create-by-typing, write,
and delete, all from userspace against a real read-write FAT32 disk.

## Files
- `kernel/fat32.c` — `fat32_delete`
- `kernel/vfs.c`, `kernel/syscall.c`, `user/shell.c` — `rm` end to end
