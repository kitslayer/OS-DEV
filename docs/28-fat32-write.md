# Milestone 28 — FAT32 write (saving files)

**Goal:** make the filesystem read-write — create files that persist on the
disk, not just read the ones baked into the image.

## Disk writes (`kernel/ata.c`)
`ata_write` mirrors `ata_read`: select the LBA, issue WRITE SECTORS (0x30), push
each sector's words out the data port (`rep outsw`), then issue a CACHE FLUSH
(0xE7) so the data actually lands on the medium.

## Creating a file (`kernel/fat32.c`)
`fat32_write(name, data, len)` does what FAT32 requires to add a file:
1. **Allocate a cluster chain** — scan the FAT for free entries (value 0),
   marking each end-of-chain, and link them (`fat_set` writes the entry in *all*
   FAT copies, preserving the reserved top bits).
2. **Write the data** into those clusters (whole sectors, zero-padded).
3. **Create a directory entry** in the root — find a free 8.3 slot, fill in the
   name (converted to 8.3), the attribute, the starting cluster, and the size,
   and write the directory sector back.

It's exposed through a new `vfs_write` op, a `SYS_writefile` syscall, and a
shell **`write <file> <text>`** command.

## What we proved
Typing `write note.txt hello` created the file, and it immediately appeared in
the Files window's live directory listing as **`NOTE.TXT (5b)`** — written to
the disk, persisting across reads (and reboots, since the image is updated).

## Files
- `kernel/ata.c` — `ata_write`
- `kernel/fat32.c` — `fat_set`, `alloc_cluster`, `to_83`, `fat32_write`
- `kernel/vfs.c`, `kernel/syscall.c`, `user/shell.c` — `write` end to end
