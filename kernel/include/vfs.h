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
};

void vfs_register(struct vfs_ops *ops);

int  vfs_list(vfs_dirent *out, int max);                 /* -1 if nothing mounted */
long vfs_read(const char *name, void *buf, unsigned long max);
long vfs_write(const char *name, const void *buf, unsigned long len);
long vfs_remove(const char *name);
long vfs_mkdir(const char *path);                        /* make a directory */
long vfs_symlink(const char *linkpath, const char *target);  /* symlink (under /tmp); 0/-1 */
int  vfs_bind(const char *from, const char *to);         /* graft FROM's subtree onto the path TO; 0/-1 */
int  vfs_binds_format(char *buf, int max);               /* list the active binds (backs /proc/binds) */
int  vfs_chdir(const char *path);                        /* change current dir */
long vfs_tree(char *out, int max);                       /* recursive listing */
void vfs_df(uint64_t *freeb, uint64_t *totalb);          /* free + total bytes */
long vfs_find(const char *want, char *out, int max);     /* recursive name search */
long vfs_rename(const char *path, const char *newname);  /* rename in place (8.3 name field only) */
