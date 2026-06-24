/* tmpfs.h — a tiny in-RAM read/write filesystem mounted at /tmp.
 *
 * Like procfs/devfs it's routed by name in the VFS (no fd table, no block
 * device), but unlike them it's WRITABLE: files live in kheap-allocated buffers
 * for the life of the session. It proves the VFS is a real multi-backend
 * abstraction (FAT32 on disk + synthetic /proc·/dev + this RAM FS), and gives a
 * fast scratch area that never touches the disk. Flat (no subdirectories). */
#pragma once
#include "vfs.h"

long tmpfs_read(const char *name, void *buf, unsigned long max);      /* bytes, or -1 */
long tmpfs_pread(const char *name, void *buf, unsigned long max, unsigned long offset);  /* positioned read; bytes/-1 (M1196) */
long tmpfs_write(const char *name, const void *data, unsigned long len); /* create/replace; len or -1 */
long tmpfs_remove(const char *name);                                 /* 0, or -1 if absent */
long tmpfs_symlink(const char *name, const char *target);            /* create a symlink; 0 or -1 */
int  tmpfs_list(vfs_dirent *out, int max);                           /* fill out[]; returns count */
int  tmpfs_stat(const char *name, int *islink, unsigned long *size, unsigned long *mtime);  /* metadata for statx; 0/-1 (M1173) */

/* Copy-on-write snapshots (Plan 9 "dump" fs): freeze the current tmpfs into a
 * generation that shares buffers with the live files until they're overwritten.
 * Routed by the VFS: read /snap (list), read /snap/<gen>/<name> (frozen file),
 * write /snap/ctl ("create" / "drop <n>"). M1115. */
long tmpfs_snap_create(void);                                        /* snapshot now; returns gen index or -1 */
long tmpfs_snap_drop(int gen);                                       /* release a snapshot; 0 or -1 */
long tmpfs_snap_read(int gen, const char *name, void *buf, unsigned long max);  /* frozen file bytes, or -1 */
int  tmpfs_snap_list(char *out, int max);                            /* /snap: generations + file counts */
long tmpfs_snap_control(const void *data, unsigned long len);        /* /snap/ctl: create / drop <n> */
