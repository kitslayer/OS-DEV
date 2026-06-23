/*
 * vfs.c — dispatch filesystem calls to whatever driver is mounted.
 *
 * Deliberately tiny: we support a single mounted filesystem, so the "VFS" is
 * just an indirection through one ops table. But it's the same shape a real
 * VFS has, and it means kmain/syscalls never mention FAT32 directly — swapping
 * in ext2 later would touch only the driver, not its callers.
 */
#include "vfs.h"
#include "procfs.h"

static struct vfs_ops *fs;

/* Synthetic /proc and /dev live alongside the mounted FS. Since the VFS is
 * name-based, we just route paths there before delegating to FAT32. A small cwd
 * flag remembers when the current directory is /proc or /dev. */
static int synth_cwd;   /* 0 = mounted FS, 1 = /proc, 2 = /dev */

static int veq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Resolve a (possibly relative) name to an absolute synthetic path. Returns 1
 * if it belongs to /proc or /dev (and fills out), else 0 (use the mounted FS). */
static int synth_path(const char *name, char *out, int max) {
    if (name[0] == '/') {
        if (!procfs_owns(name)) return 0;
        int i = 0; while (name[i] && i < max - 1) { out[i] = name[i]; i++; } out[i] = 0;
        return 1;
    }
    if (synth_cwd == 0) return 0;
    const char *base = synth_cwd == 1 ? "/proc/" : "/dev/";
    int p = 0; while (base[p] && p < max - 1) { out[p] = base[p]; p++; }
    int i = 0; while (name[i] && p < max - 1) out[p++] = name[i++];
    out[p] = 0;
    return 1;
}

void vfs_register(struct vfs_ops *ops) { fs = ops; }

int vfs_list(vfs_dirent *out, int max) {
    if (synth_cwd) return procfs_list(synth_cwd == 1 ? "/proc" : "/dev", out, max);
    return fs ? fs->list(out, max) : -1;
}

long vfs_read(const char *name, void *buf, unsigned long max) {
    char ap[96];
    if (synth_path(name, ap, sizeof ap)) return procfs_read(ap, buf, max);
    return fs ? fs->read(name, buf, max) : -1;
}

long vfs_write(const char *name, const void *buf, unsigned long len) {
    char ap[96];
    if (synth_path(name, ap, sizeof ap)) { long r = procfs_write(ap, buf, len); return r == -2 ? -1 : r; }
    return (fs && fs->write) ? fs->write(name, buf, len) : -1;
}

long vfs_remove(const char *name) {
    return (fs && fs->remove) ? fs->remove(name) : -1;
}

long vfs_mkdir(const char *path) {
    return (fs && fs->mkdir) ? fs->mkdir(path) : -1;
}

int vfs_chdir(const char *path) {
    if (veq(path, "/proc")) { synth_cwd = 1; return 0; }   /* enter synthetic dirs */
    if (veq(path, "/dev"))  { synth_cwd = 2; return 0; }
    synth_cwd = 0;                                          /* any other cd leaves them */
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
