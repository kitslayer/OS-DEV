# Milestone 10 — VFS + FAT32 filesystem

**Goal:** read named files off a real disk. This adds the last classic kernel
subsystem and ties everything together: by the end you can type `cat hello.txt`
in the shell and the bytes travel from a virtual disk all the way to ring 3.

## The layers (bottom to top)

```
  disk image (build/fat.img)
        │
   ATA PIO driver  (kernel/ata.c)      — read 512-byte sectors by LBA number
        │
   FAT32 driver    (kernel/fat32.c)    — turn sectors into files & directories
        │
   VFS             (kernel/vfs.c)      — a generic "list / read" interface
        │
   syscalls        (SYS_list, SYS_readfile)
        │
   shell `ls` / `cat`   (ring 3)
```

### 1. The block device (`kernel/ata.c`)
The disk is read in fixed **512-byte sectors** addressed by a linear number
(**LBA**). The ATA driver talks to the IDE controller's I/O ports (0x1F0–0x1F7):
select the drive, write the LBA and sector count, issue READ, then poll the
status register and pull each sector's 256 words through the data port. "PIO"
(the CPU moves every word) is slow but simple.

### 2. The filesystem (`kernel/fat32.c`)
FAT32's layout is `[reserved][FAT #1][FAT #2][data clusters]`. The **File
Allocation Table** is an array indexed by cluster number; `FAT[c]` is the *next*
cluster of a file (or an end-of-chain marker). So **a file is a linked list of
clusters threaded through the FAT**, and a directory is just a file whose data
is 32-byte entries (an 8.3 name, attributes, starting cluster, size). To read a
file we find its directory entry, then follow the cluster chain, converting each
cluster to a sector and reading it.

### 3. The VFS (`kernel/vfs.c`)
A deliberately thin abstraction: a filesystem driver registers a `{list, read}`
ops table, and the VFS forwards calls to it. The point is that `kmain` and the
syscalls say "read this file" without naming FAT32 — swapping in ext2 later
would touch only the driver.

### 4. Reaching userspace
Two syscalls (`SYS_list`, `SYS_readfile`) expose the VFS to ring 3, and the
shell gains `ls` and `cat`. Now a user program reads files exactly the way it
reads anything else from our kernel — through the syscall boundary.

## Building the disk without mtools

`mtools`/loopback-mount weren't available, so `tools/mkfatfs.c` is a small
**host** program that writes a valid FAT32 image (boot sector/BPB, both FATs,
root directory, file data) with a few files baked in. Building both the writer
and the reader guarantees the on-disk layout matches.

## A bug worth remembering

`cat` worked immediately but `ls` printed nothing. The cause was a **syscall ABI
mismatch**: the kernel read `SYS_list`'s buffer from `rsi`/`rdx` (the 2nd/3rd
arg registers, matching `write`), but the userspace wrapper passed it as the
1st argument (`rdi`). `cat`'s three arguments happened to line up; `ls`'s didn't.
The fix was to pass a dummy leading argument so the buffer landed in the
register the kernel expected — a reminder that *both sides must agree on the
exact ABI*, register for register.

## Files
- `tools/mkfatfs.c` — host-side FAT32 image builder
- `kernel/ata.c` — ATA PIO sector reads
- `kernel/fat32.c` — FAT32 directory walk + file read
- `kernel/vfs.c` — the mount/dispatch layer
- shell `ls`/`cat` via `SYS_list` / `SYS_readfile`
