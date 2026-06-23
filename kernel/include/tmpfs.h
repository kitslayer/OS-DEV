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
long tmpfs_write(const char *name, const void *data, unsigned long len); /* create/replace; len or -1 */
long tmpfs_remove(const char *name);                                 /* 0, or -1 if absent */
int  tmpfs_list(vfs_dirent *out, int max);                           /* fill out[]; returns count */
