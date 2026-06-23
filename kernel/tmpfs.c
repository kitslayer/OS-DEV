/*
 * tmpfs.c — an in-RAM read/write filesystem at /tmp.
 *
 * A fixed table of files, each a kheap-allocated buffer. The VFS routes /tmp
 * paths here (see kernel/vfs.c), so `echo hi > /tmp/x`, `cat /tmp/x`, `ls /tmp`
 * and `rm /tmp/x` all work against RAM instead of the disk. Writable, unlike the
 * synthetic /proc and /dev — which is the point: it shows the name-based VFS can
 * carry a third, independent backend. Flat namespace (no subdirectories).
 */
#include "tmpfs.h"
#include "kheap.h"
#include "vfs.h"                     /* vfs_read, to follow a symlink to its target */
#include <stdint.h>

#define TMPFS_MAX 32                 /* up to 32 files */

/* `link`: this entry is a symbolic link — `buf` holds the NUL-terminated target
 * PATH (not file data), and a read follows it (M1081). */
static struct { char name[64]; char *buf; uint32_t len; int used; int link; } tf[TMPFS_MAX];
static int resolve_depth;            /* bounds symlink->symlink chains (loop guard) */

static int teq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void tcpy(char *d, const char *s) { int i = 0; while (s[i] && i < 63) { d[i] = s[i]; i++; } d[i] = 0; }
static int tfind(const char *name) {
    for (int i = 0; i < TMPFS_MAX; i++) if (tf[i].used && teq(tf[i].name, name)) return i;
    return -1;
}

long tmpfs_read(const char *name, void *buf, unsigned long max) {
    int i = tfind(name);
    if (i < 0) return -1;
    if (tf[i].link) {                         /* a symlink: read its target instead */
        if (resolve_depth >= 8) return -1;    /* loop / too many hops */
        resolve_depth++;
        long r = vfs_read(tf[i].buf, buf, max);   /* buf holds the target path */
        resolve_depth--;
        return r;
    }
    unsigned long n = tf[i].len; if (n > max) n = max;
    for (unsigned long k = 0; k < n; k++) ((char *)buf)[k] = tf[i].buf[k];
    return (long)n;
}

/* Create (or replace) a symlink `name` -> `target`. The link stores the target
 * path as its content; tmpfs_read follows it. Returns 0 / -1. */
long tmpfs_symlink(const char *name, const char *target) {
    int i = tfind(name);
    if (i < 0) { for (i = 0; i < TMPFS_MAX; i++) if (!tf[i].used) break; if (i == TMPFS_MAX) return -1; }
    int tl = 0; while (target[tl]) tl++;
    char *nb = (char *)kmalloc(tl + 1);
    if (!nb) return -1;
    for (int k = 0; k <= tl; k++) nb[k] = target[k];   /* copy incl. the NUL */
    if (tf[i].used && tf[i].buf) kfree(tf[i].buf);
    tf[i].buf = nb; tf[i].len = (uint32_t)tl; tf[i].link = 1; tf[i].used = 1; tcpy(tf[i].name, name);
    return 0;
}

long tmpfs_write(const char *name, const void *data, unsigned long len) {
    int i = tfind(name);
    if (i < 0) { for (i = 0; i < TMPFS_MAX; i++) if (!tf[i].used) break; if (i == TMPFS_MAX) return -1; }
    char *nb = (char *)kmalloc(len ? len : 1);    /* a fresh buffer (write replaces the whole file) */
    if (!nb) return -1;
    for (unsigned long k = 0; k < len; k++) nb[k] = ((const char *)data)[k];
    if (tf[i].used && tf[i].buf) kfree(tf[i].buf);
    tf[i].buf = nb; tf[i].len = (uint32_t)len; tf[i].used = 1; tf[i].link = 0; tcpy(tf[i].name, name);
    return (long)len;
}

long tmpfs_remove(const char *name) {
    int i = tfind(name);
    if (i < 0) return -1;
    if (tf[i].buf) kfree(tf[i].buf);
    tf[i].buf = 0; tf[i].len = 0; tf[i].used = 0; tf[i].link = 0;
    return 0;
}

int tmpfs_list(vfs_dirent *out, int max) {
    int n = 0;
    for (int i = 0; i < TMPFS_MAX && n < max; i++) if (tf[i].used) {
        int k = 0; while (tf[i].name[k] && k < 62) { out[n].name[k] = tf[i].name[k]; k++; }
        out[n].name[k] = 0; out[n].size = tf[i].len; out[n].date = out[n].time = 0; n++;
    }
    return n;
}
