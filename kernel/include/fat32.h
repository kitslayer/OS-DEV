/* fat32.h — read-only FAT32 driver. */
#pragma once

/* Read the boot sector from the ATA disk, validate FAT32, and register with
 * the VFS. Returns 0 on success, -1 if no valid FAT32 volume was found. */
int fat32_mount(void);
