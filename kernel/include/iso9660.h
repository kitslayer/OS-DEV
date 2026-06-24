/*
 * iso9660.h — a read-only ISO 9660 filesystem driver (CD/DVD images).
 *
 * Device-agnostic, mirroring the FAT32/ext2 volume API (partition.h / ext2.h):
 * each call takes a blk_read_fn + ctx + the volume's absolute start LBA. Parses
 * the Primary Volume Descriptor (logical sector 16, "CD001" magic), the root
 * directory record, and the chain of directory records, so a real .iso image
 * (e.g. from mkisofs/genisoimage) can be mounted and read in-guest. ISO 9660
 * speaks 2048-byte logical sectors over the 512-byte block device. Names are
 * upper-cased and may carry a ";1" version suffix, which we strip. Read-only;
 * bounded; never writes — the perfect safe filesystem.
 */
#pragma once
#include <stdint.h>
#include "partition.h"   /* blk_read_fn, fatvol_dirent (reused for listings) */

int  iso9660_probe(blk_read_fn read, void *ctx, uint64_t start_lba);   /* 0 if a valid ISO 9660 volume, else -1 */
int  iso9660_list_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path,
                       fatvol_dirent *out, int max);                   /* list a dir; entries written, or -1 */
long iso9660_read_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path,
                       void *buf, unsigned long max);                  /* read a file; bytes, or -1 */
int  iso9660_isdir_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path);  /* 1 dir / 0 file / -1 absent */
