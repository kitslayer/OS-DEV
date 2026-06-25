/*
 * vfs.h — a thin virtual filesystem layer.
 *
 * The VFS is an abstraction: the rest of the kernel asks "list the directory"
 * or "read this file" without knowing or caring that the answer comes from
 * FAT32 (vs ext2, vs a network FS...). A filesystem driver registers an ops
 * table; the VFS forwards calls to whatever is mounted.
 */
#pragma once
#include <stdint.h>
#include "partition.h"   /* ext2_extent_t, for vfs_fiemap (M1152) */

typedef struct {
    char     name[64];
    uint32_t size;
    uint16_t date, time;     /* FAT-packed last-write date/time (0 = unset) */
} vfs_dirent;

struct vfs_ops {
    int  (*list)(vfs_dirent *out, int max);
    long (*read)(const char *name, void *buf, unsigned long max);
    long (*write)(const char *name, const void *buf, unsigned long len);
    long (*remove)(const char *name);
    long (*mkdir)(const char *path);
    int  (*chdir)(const char *path);
    long (*tree)(char *out, int max);
    void (*df)(uint64_t *freeb, uint64_t *totalb);
    long (*find)(const char *want, char *out, int max);
    long (*rename)(const char *path, const char *newname);   /* change a name in place (8.3) */
    long (*pread)(const char *name, void *buf, unsigned long max, uint64_t off);  /* offset read for file-backed mmap (M1136); may be NULL */
};
/* Offset read of a regular file (boot FS), for file-backed mmap's demand faults.
 * Bytes read (0 past EOF), or -1 if unsupported/absent. M1136. */
long vfs_pread(const char *name, void *buf, unsigned long max, uint64_t off);
/* Mount a union overlay at /over: reads fall through LOWER->...->UPPER, writes
 * copy-up to UPPER (the lower stays read-only). One at a time. M1142. */
void vfs_overlay_mount(const char *lower, const char *upper);
/* per-process cwd (M1144): swap the live cwd to the calling app (call at syscall
 * entry); inherit the parent's cwd into a fork child; forget an exiting app. */
struct app;
void vfs_sync_cwd(void);
void vfs_cwd_inherit(struct app *child);
void vfs_cwd_forget(struct app *a);

void vfs_register(struct vfs_ops *ops);

int  vfs_list(vfs_dirent *out, int max);                 /* -1 if nothing mounted */
long vfs_read(const char *name, void *buf, unsigned long max);
long vfs_write(const char *name, const void *buf, unsigned long len);
long vfs_remove(const char *name);
long vfs_mkdir(const char *path);                        /* make a directory */
long vfs_symlink(const char *linkpath, const char *target);  /* symlink (under /tmp); 0/-1 */
long vfs_readlink(const char *path, void *buf, unsigned long max);  /* read a symlink's target, not followed; bytes/-1 (M1233) */
long vfs_link(const char *oldpath, const char *newpath);     /* hard link (same ext2 mount); 0/-1 (M1207) */
long vfs_rename_path(const char *oldpath, const char *newpath);  /* rename/move within one ext2 mount; 0/-1 (M1213) */
long vfs_rename2(const char *oldpath, const char *newpath, int flags); /* renameat2 NOREPLACE/EXCHANGE (M1232) */
long vfs_truncate(const char *path, uint64_t newlen);            /* resize a real file (tmpfs / ext2 mount); 0/-1 (M1228) */
long vfs_seek_data_hole(const char *path, long off, int find_hole); /* SEEK_HOLE/DATA: next hole/data >= off; -1 ENXIO (M1229) */
long vfs_utimes(const char *path, long atime, long mtime);       /* set atime/mtime (neg = leave); 0/-1 (M1230) */
long vfs_chmod(const char *path, uint32_t mode);                 /* set perm bits (ext2 mounts); 0/-1 (M1241) */
struct statx;
int  vfs_stat(const char *path, struct statx *st);      /* file metadata for statx; 0/-1 (M1173) */
int  vfs_fiemap(const char *path, ext2_extent_t *out, int max);  /* file physical extent map (ext2 mounts); count/-1 (M1152) */
long vfs_punch_hole(const char *path, uint64_t offset, uint64_t len);  /* fallocate PUNCH_HOLE (ext2 mounts); blocks/-1 (M1153) */
long vfs_setxattr(const char *path, const char *name, const void *val, unsigned long vlen);  /* set user.* xattr (ext2 mounts); vlen/-1 (M1182) */
long vfs_getxattr(const char *path, const char *name, void *out, unsigned long max);  /* get user.* xattr (ext2 mounts); size/-1 (M1182) */
long vfs_listxattr(const char *path, char *out, unsigned long max);  /* NUL-sep xattr names (ext2 mounts); total/-1 (M1182) */
long vfs_removexattr(const char *path, const char *name);  /* remove a user.* xattr (ext2 mounts); 0/-1 (M1182) */
int  vfs_bind(const char *from, const char *to);         /* graft FROM's subtree onto the path TO; 0/-1 */
int  vfs_binds_format(char *buf, int max);               /* list the active binds (backs /proc/binds) */
int  vfs_unshare(void);                                  /* detach the caller into a private mount namespace; 0/-1 (M1122) */
int  vfs_ready(const char *name);                        /* non-blocking: would a read of `name` not block? (fswait, M1125) */
int  vfs_chdir(const char *path);                        /* change current dir */
long vfs_tree(char *out, int max);                       /* recursive listing */
void vfs_df(uint64_t *freeb, uint64_t *totalb);          /* free + total bytes */
long vfs_find(const char *want, char *out, int max);     /* recursive name search */
long vfs_rename(const char *path, const char *newname);  /* rename in place (8.3 name field only) */
