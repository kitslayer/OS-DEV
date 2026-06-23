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
#include "blockdev.h"

static struct vfs_ops *fs;

/* Synthetic /proc + /dev AND read-only secondary-disk mounts (/disk1, /disk2, …)
 * live alongside the mounted boot FS. Since the VFS is name-based, we just route
 * those paths before delegating to FAT32. A small cwd flag remembers when the
 * current directory is one of them. */
static int synth_cwd;   /* 0 = boot FS, 1 = /proc, 2 = /dev, >=3 = mount (synth_cwd-3) */

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

/* Route a (possibly relative) name to a mounted disk volume. Returns 1 + sets
 * *midx (mount index) and *fname (the file within it) if it targets /disk<N>,
 * else 0 (use /proc·/dev·boot FS). Mounts are root-only (no subdir paths). */
static int mount_path(const char *name, int *midx, const char **fname) {
    if (name[0] == '/') {
        char comp[12]; int c = 0; const char *p = name + 1;
        while (*p && *p != '/' && c < 11) comp[c++] = *p++;
        comp[c] = 0;
        int idx = blockdev_mount_index(comp);
        if (idx < 0) return 0;
        *midx = idx; *fname = (*p == '/') ? p + 1 : p;
        return 1;
    }
    if (synth_cwd >= 3) { *midx = synth_cwd - 3; *fname = name; return 1; }
    return 0;
}

void vfs_register(struct vfs_ops *ops) { fs = ops; }

int vfs_list(vfs_dirent *out, int max) {
    if (synth_cwd == 1 || synth_cwd == 2)
        return procfs_list(synth_cwd == 1 ? "/proc" : "/dev", out, max);
    if (synth_cwd >= 3) {                                  /* a mounted disk volume's root */
        fatvol_dirent fe[64];
        int cap = max < 64 ? max : 64;
        int n = blockdev_mount_list(synth_cwd - 3, fe, cap);
        for (int i = 0; i < n; i++) {
            int k = 0; while (fe[i].name[k] && k < 60) { out[i].name[k] = fe[i].name[k]; k++; }
            if (fe[i].is_dir) out[i].name[k++] = '/';     /* match fat32_list's dir marker */
            out[i].name[k] = 0;
            out[i].size = fe[i].size; out[i].date = 0; out[i].time = 0;
        }
        return n;
    }
    return fs ? fs->list(out, max) : -1;
}

long vfs_read(const char *name, void *buf, unsigned long max) {
    char ap[96];
    if (synth_path(name, ap, sizeof ap)) return procfs_read(ap, buf, max);
    int midx; const char *fname;
    if (mount_path(name, &midx, &fname)) return blockdev_mount_read(midx, fname, buf, max);
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
    if (path[0] == '/') {                                  /* enter a mounted disk: /diskN */
        int idx = blockdev_mount_index(path + 1);
        if (idx >= 0) { synth_cwd = 3 + idx; return 0; }
    }
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
