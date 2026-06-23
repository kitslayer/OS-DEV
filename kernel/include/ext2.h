/*
 * ext2.h — a read-only ext2 filesystem driver (Linux interop).
 *
 * Device-agnostic, mirroring the FAT32 volume API (partition.h): each call
 * takes a blk_read_fn + ctx + the volume's absolute start LBA. Parses the
 * superblock, block-group descriptors, inodes (direct + single/double-indirect
 * blocks) and linked directory entries, so a real mke2fs image can be mounted
 * and read in-guest. Read-only; bounded; never writes.
 */
#pragma once
#include <stdint.h>
#include "partition.h"   /* blk_read_fn, fatvol_dirent (reused for listings) */

int  ext2_probe(blk_read_fn read, void *ctx, uint64_t start_lba);   /* 0 if a valid ext2 volume, else -1 */
int  ext2_list_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path,
                    fatvol_dirent *out, int max);                   /* list a dir; entries written, or -1 */
long ext2_read_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path,
                    void *buf, unsigned long max);                  /* read a file; bytes, or -1 */
int  ext2_isdir_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path);  /* 1 dir / 0 file / -1 absent */
