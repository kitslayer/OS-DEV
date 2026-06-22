/*
 * vfs.c — dispatch filesystem calls to whatever driver is mounted.
 *
 * Deliberately tiny: we support a single mounted filesystem, so the "VFS" is
 * just an indirection through one ops table. But it's the same shape a real
 * VFS has, and it means kmain/syscalls never mention FAT32 directly — swapping
 * in ext2 later would touch only the driver, not its callers.
 */
#include "vfs.h"

static struct vfs_ops *fs;

void vfs_register(struct vfs_ops *ops) { fs = ops; }

int vfs_list(vfs_dirent *out, int max) {
    return fs ? fs->list(out, max) : -1;
}

long vfs_read(const char *name, void *buf, unsigned long max) {
    return fs ? fs->read(name, buf, max) : -1;
}

long vfs_write(const char *name, const void *buf, unsigned long len) {
    return (fs && fs->write) ? fs->write(name, buf, len) : -1;
}

long vfs_remove(const char *name) {
    return (fs && fs->remove) ? fs->remove(name) : -1;
}

long vfs_mkdir(const char *path) {
    return (fs && fs->mkdir) ? fs->mkdir(path) : -1;
}

int vfs_chdir(const char *path) {
    return (fs && fs->chdir) ? fs->chdir(path) : -1;
}

long vfs_tree(char *out, int max) {
    return (fs && fs->tree) ? fs->tree(out, max) : -1;
}

void vfs_df(uint64_t *freeb, uint64_t *totalb) {
    if (fs && fs->df) fs->df(freeb, totalb);
    else { *freeb = 0; *totalb = 0; }
}

long vfs_find(const char *want, char *out, int max) {
    return (fs && fs->find) ? fs->find(want, out, max) : -1;
}

long vfs_rename(const char *path, const char *newname) {
    return (fs && fs->rename) ? fs->rename(path, newname) : -1;
}
