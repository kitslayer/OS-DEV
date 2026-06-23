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
#include <stdint.h>

#define TMPFS_MAX 32                 /* up to 32 files */

static struct { char name[64]; char *buf; uint32_t len; int used; } tf[TMPFS_MAX];

static int teq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void tcpy(char *d, const char *s) { int i = 0; while (s[i] && i < 63) { d[i] = s[i]; i++; } d[i] = 0; }
static int tfind(const char *name) {
    for (int i = 0; i < TMPFS_MAX; i++) if (tf[i].used && teq(tf[i].name, name)) return i;
    return -1;
}

long tmpfs_read(const char *name, void *buf, unsigned long max) {
    int i = tfind(name);
    if (i < 0) return -1;
    unsigned long n = tf[i].len; if (n > max) n = max;
    for (unsigned long k = 0; k < n; k++) ((char *)buf)[k] = tf[i].buf[k];
    return (long)n;
}

long tmpfs_write(const char *name, const void *data, unsigned long len) {
    int i = tfind(name);
    if (i < 0) { for (i = 0; i < TMPFS_MAX; i++) if (!tf[i].used) break; if (i == TMPFS_MAX) return -1; }
    char *nb = (char *)kmalloc(len ? len : 1);    /* a fresh buffer (write replaces the whole file) */
    if (!nb) return -1;
    for (unsigned long k = 0; k < len; k++) nb[k] = ((const char *)data)[k];
    if (tf[i].used && tf[i].buf) kfree(tf[i].buf);
    tf[i].buf = nb; tf[i].len = (uint32_t)len; tf[i].used = 1; tcpy(tf[i].name, name);
    return (long)len;
}

long tmpfs_remove(const char *name) {
    int i = tfind(name);
    if (i < 0) return -1;
    if (tf[i].buf) kfree(tf[i].buf);
    tf[i].buf = 0; tf[i].len = 0; tf[i].used = 0;
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
