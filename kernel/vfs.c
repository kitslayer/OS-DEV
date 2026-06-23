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
#include "tmpfs.h"
#include "fsevents.h"
#include "mbox.h"

static struct vfs_ops *fs;

/* Synthetic /proc + /dev, a writable RAM /tmp, AND read-only secondary-disk
 * mounts (/disk1, /disk2, …) all live alongside the mounted boot FS. Since the
 * VFS is name-based, we route those paths before delegating to FAT32. A small
 * cwd flag remembers which one the current directory is. */
static int synth_cwd;   /* 0 = boot FS, 1 = /proc, 2 = /dev, 3 = /tmp, >=4 = mount (synth_cwd-4) */
/* When synth_cwd >= 4 (inside a mounted disk), the current directory WITHIN that
 * volume, relative to its root with no leading '/'. "" = the volume root (M1070). */
static char mount_sub[128];

static int veq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int vstarts(const char *s, const char *pre) { while (*pre) { if (*s++ != *pre++) return 0; } return 1; }

/* --- bind mounts (M1091): graft one path onto another in the namespace --------
 * `bind FROM TO` makes the path TO also resolve to FROM (Plan 9 style). Because
 * the VFS is pure name-routing, this is a single rewrite step applied to every
 * absolute path BEFORE the /proc·/dev·/tmp·/diskN routing — TO and everything
 * under it is transparently redirected to FROM. One pass only (no recursion), so
 * even a cyclic bind can't loop. Global for now. */
#define NBINDS 8
static struct { char from[64], to[64]; int used; } binds[NBINDS];

int vfs_bind(const char *from, const char *to) {
    if (!from || !to || from[0] != '/' || to[0] != '/') return -1;
    int slot = -1;
    for (int i = 0; i < NBINDS; i++) if (!binds[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    int i = 0; while (from[i] && i < 63) { binds[slot].from[i] = from[i]; i++; } binds[slot].from[i] = 0;
    int j = 0; while (to[j] && j < 63) { binds[slot].to[j] = to[j]; j++; } binds[slot].to[j] = 0;
    binds[slot].used = 1;
    return 0;
}

/* Rewrite an absolute `name` through the longest-matching bind (TO -> FROM) into
 * out[max]; returns `name` unchanged (not copied) if nothing matches. */
static const char *bind_resolve(const char *name, char *out, int max) {
    if (name[0] != '/') return name;                /* binds are absolute */
    int best = -1, bestlen = 0;
    for (int i = 0; i < NBINDS; i++) {
        if (!binds[i].used) continue;
        int tl = 0; while (binds[i].to[tl]) tl++;
        if (!vstarts(name, binds[i].to)) continue;
        if (name[tl] != 0 && name[tl] != '/') continue;   /* must match a whole component */
        if (tl > bestlen) { best = i; bestlen = tl; }
    }
    if (best < 0) return name;
    int p = 0; const char *f = binds[best].from;
    while (*f && p < max - 1) out[p++] = *f++;
    for (const char *rest = name + bestlen; *rest && p < max - 1; rest++) out[p++] = *rest;
    out[p] = 0;
    return out;
}

int vfs_binds_format(char *b, int max) {            /* backs /proc/binds */
    int p = 0; const char *hdr = "  TO                    FROM\n";
    while (*hdr && p < max - 1) b[p++] = *hdr++;
    int any = 0;
    for (int i = 0; i < NBINDS; i++) if (binds[i].used) {
        if (p < max - 1) b[p++] = ' '; if (p < max - 1) b[p++] = ' ';
        int tl = 0; const char *t = binds[i].to; while (*t && p < max - 1) { b[p++] = *t++; tl++; }
        for (int k = tl; k < 22 && p < max - 1; k++) b[p++] = ' ';
        const char *f = binds[i].from; while (*f && p < max - 1) b[p++] = *f++;
        if (p < max - 1) b[p++] = '\n';
        any = 1;
    }
    if (!any) { const char *m = "  (none — `bind <from> <to>`)\n"; while (*m && p < max - 1) b[p++] = *m++; }
    if (p < max) b[p] = 0;
    return p;
}

/* Join a relative path `rel` onto the volume-relative base `cur`, collapsing "."
 * and ".." components, into `out` (no leading '/', "" = the volume root). Used to
 * track the cwd as the user descends/ascends a mounted disk's subdirectories. */
static void mount_sub_join(const char *cur, const char *rel, char *out, int max) {
    int p = 0;
    for (int i = 0; cur[i] && p < max - 1; i++) out[p++] = cur[i];
    out[p] = 0;
    const char *s = rel;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *c = s; int len = 0;
        while (s[len] && s[len] != '/') len++;
        s += len;
        if (len == 1 && c[0] == '.') continue;                 /* "." : stay */
        if (len == 2 && c[0] == '.' && c[1] == '.') {          /* ".." : pop a component */
            while (p > 0 && out[p - 1] != '/') p--;
            if (p > 0) p--;                                    /* drop the separator */
            out[p] = 0;
            continue;
        }
        if (p > 0 && p < max - 1) out[p++] = '/';
        for (int i = 0; i < len && p < max - 1; i++) out[p++] = c[i];
        out[p] = 0;
    }
}

/* Route an absolute /ipc/<name> path to a named message queue (mbox). Returns 1
 * + the queue name if it targets /ipc/, else 0. Absolute-only (no cwd). */
static int ipc_path(const char *name, const char **q) {
    if (!vstarts(name, "/ipc/")) return 0;
    *q = name + 5;
    return **q != 0;
}

/* Route a (possibly relative) name to the RAM /tmp filesystem. Returns 1 + the
 * basename within /tmp if it targets /tmp, else 0. */
static int tmp_path(const char *name, const char **base) {
    if (name[0] == '/') { if (!vstarts(name, "/tmp/")) return 0; *base = name + 5; return **base != 0; }
    if (synth_cwd == 3) { *base = name; return 1; }
    return 0;
}

/* Resolve a (possibly relative) name to an absolute synthetic path. Returns 1
 * if it belongs to /proc or /dev (and fills out), else 0 (use the mounted FS). */
static int synth_path(const char *name, char *out, int max) {
    if (name[0] == '/') {
        if (!procfs_owns(name)) return 0;
        int i = 0; while (name[i] && i < max - 1) { out[i] = name[i]; i++; } out[i] = 0;
        return 1;
    }
    if (synth_cwd != 1 && synth_cwd != 2) return 0;   /* /tmp + disk mounts route elsewhere */
    const char *base = synth_cwd == 1 ? "/proc/" : "/dev/";
    int p = 0; while (base[p] && p < max - 1) { out[p] = base[p]; p++; }
    int i = 0; while (name[i] && p < max - 1) out[p++] = name[i++];
    out[p] = 0;
    return 1;
}

/* Route a (possibly relative) name to a mounted disk volume. Returns 1 + sets
 * *midx (mount index) and fills `path` with the file's location relative to the
 * volume root if it targets /disk<N> (absolute) or we're cwd'd inside a mount
 * (relative, resolved against mount_sub); else 0 (use /proc·/dev·boot FS).
 * Subdirectory-aware (M1070): /diskN/a/b/file and relative names both resolve. */
static int mount_path(const char *name, int *midx, char *path, int max) {
    if (name[0] == '/') {
        char comp[12]; int c = 0; const char *p = name + 1;
        while (*p && *p != '/' && c < 11) comp[c++] = *p++;
        comp[c] = 0;
        int idx = blockdev_mount_index(comp);
        if (idx < 0) return 0;
        *midx = idx;
        const char *rest = (*p == '/') ? p + 1 : p;
        int i = 0; while (rest[i] && i < max - 1) { path[i] = rest[i]; i++; } path[i] = 0;
        return 1;
    }
    if (synth_cwd >= 4) {                          /* relative name inside the cwd mount */
        *midx = synth_cwd - 4;
        int p = 0;
        for (int i = 0; mount_sub[i] && p < max - 1; i++) path[p++] = mount_sub[i];
        if (p > 0 && p < max - 1) path[p++] = '/';
        for (int i = 0; name[i] && p < max - 1; i++) path[p++] = name[i];
        path[p] = 0;
        return 1;
    }
    return 0;
}

void vfs_register(struct vfs_ops *ops) { fs = ops; }

int vfs_list(vfs_dirent *out, int max) {
    if (synth_cwd == 1 || synth_cwd == 2)
        return procfs_list(synth_cwd == 1 ? "/proc" : "/dev", out, max);
    if (synth_cwd == 3) return tmpfs_list(out, max);       /* the RAM /tmp */
    if (synth_cwd >= 4) {                                  /* a mounted disk volume's root */
        fatvol_dirent fe[64];
        int cap = max < 64 ? max : 64;
        int n = blockdev_mount_list(synth_cwd - 4, mount_sub, fe, cap);
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
    char ap[96]; const char *tb;
    char rb[160]; name = bind_resolve(name, rb, sizeof rb);     /* bind mounts (M1091) */
    if (synth_path(name, ap, sizeof ap)) return procfs_read(ap, buf, max);
    if (ipc_path(name, &tb)) return mbox_read(tb, buf, max);   /* /ipc/<q>: dequeue a message (blocks if empty) */
    if (tmp_path(name, &tb)) return tmpfs_read(tb, buf, max);
    int midx; char fpath[192];
    if (mount_path(name, &midx, fpath, sizeof fpath)) return blockdev_mount_read(midx, fpath, buf, max);
    return fs ? fs->read(name, buf, max) : -1;
}

long vfs_write(const char *name, const void *buf, unsigned long len) {
    char ap[96]; const char *tb;
    char rb[160]; name = bind_resolve(name, rb, sizeof rb);     /* bind mounts (M1091) */
    if (synth_path(name, ap, sizeof ap)) { long r = procfs_write(ap, buf, len); return r == -2 ? -1 : r; }
    if (ipc_path(name, &tb)) return mbox_write(tb, buf, len);   /* /ipc/<q>: enqueue a message */
    long r;
    if (tmp_path(name, &tb)) r = tmpfs_write(tb, buf, len);
    else {
        int midx; char fpath[192];
        if (mount_path(name, &midx, fpath, sizeof fpath)) return -1;   /* disk mounts are read-only */
        r = (fs && fs->write) ? fs->write(name, buf, len) : -1;
    }
    if (r >= 0) fsevents_record('w', name);    /* a real file changed (M1085) */
    return r;
}

long vfs_remove(const char *name) {
    const char *tb;
    char rb[160]; name = bind_resolve(name, rb, sizeof rb);     /* bind mounts (M1091) */
    long r;
    if (tmp_path(name, &tb)) r = tmpfs_remove(tb);
    else {
        int midx; char fpath[192];
        if (mount_path(name, &midx, fpath, sizeof fpath)) return -1;   /* disk mounts are read-only */
        r = (fs && fs->remove) ? fs->remove(name) : -1;
    }
    if (r >= 0) fsevents_record('d', name);
    return r;
}

long vfs_mkdir(const char *path) {
    long r = (fs && fs->mkdir) ? fs->mkdir(path) : -1;
    if (r >= 0) fsevents_record('m', path);
    return r;
}

/* Create a symlink `linkpath` -> `target`. Only the RAM /tmp backend supports
 * links (the synthetic /proc·/dev are read-only; FAT32 has no native symlink),
 * so the link must live under /tmp. Returns 0 / -1 (M1081). */
long vfs_symlink(const char *linkpath, const char *target) {
    const char *base;
    if (tmp_path(linkpath, &base)) {
        long r = tmpfs_symlink(base, target);
        if (r >= 0) fsevents_record('l', linkpath);
        return r;
    }
    return -1;
}

/* Copy a validated subpath into mount_sub (bounded). */
static void set_mount_sub(const char *sub) {
    int i = 0; while (sub[i] && i < (int)sizeof mount_sub - 1) { mount_sub[i] = sub[i]; i++; }
    mount_sub[i] = 0;
}

int vfs_chdir(const char *path) {
    char rb[160]; path = bind_resolve(path, rb, sizeof rb);     /* bind mounts (M1091) */
    if (veq(path, "/proc")) { synth_cwd = 1; return 0; }   /* enter synthetic dirs */
    if (veq(path, "/dev"))  { synth_cwd = 2; return 0; }
    if (veq(path, "/tmp"))  { synth_cwd = 3; return 0; }   /* the RAM filesystem */
    if (path[0] == '/') {                                  /* enter a mounted disk: /diskN[/sub...] */
        char comp[12]; int c = 0; const char *p = path + 1;
        while (*p && *p != '/' && c < 11) comp[c++] = *p++;
        comp[c] = 0;
        int idx = blockdev_mount_index(comp);
        if (idx >= 0) {
            const char *rest = (*p == '/') ? p + 1 : p;
            char sub[128]; mount_sub_join("", rest, sub, sizeof sub);
            if (sub[0] == 0 || blockdev_mount_isdir(idx, sub)) {   /* root, or a real subdir */
                synth_cwd = 4 + idx; set_mount_sub(sub);
                return 0;
            }
            return -1;                                     /* /diskN/<not a directory> */
        }
    } else if (synth_cwd >= 4) {                           /* relative cd inside a mounted disk */
        if (mount_sub[0] == 0 && veq(path, "..")) {        /* ".." at the volume root: leave the mount */
            synth_cwd = 0;
            return (fs && fs->chdir) ? fs->chdir(path) : -1;
        }
        char sub[128]; mount_sub_join(mount_sub, path, sub, sizeof sub);
        if (sub[0] == 0 || blockdev_mount_isdir(synth_cwd - 4, sub)) {
            set_mount_sub(sub);
            return 0;
        }
        return -1;                                         /* no such subdirectory */
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
    long r = (fs && fs->rename) ? fs->rename(path, newname) : -1;
    if (r >= 0) fsevents_record('r', path);
    return r;
}
