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
#include "rtc.h"                     /* rtc_unix, for mtime (M1173) */
#include <stdint.h>

#define TMPFS_MAX 32                 /* up to 32 files */

/* `link`: this entry is a symbolic link — `buf` holds the NUL-terminated target
 * PATH (not file data), and a read follows it (M1081). `mtime`: Unix epoch
 * seconds of the last write/create, for statx (M1173). */
static struct { char name[64]; char *buf; uint32_t len; int used; int link; uint32_t mtime; } tf[TMPFS_MAX];
static int resolve_depth;            /* bounds symlink->symlink chains (loop guard) */

static int teq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void tcpy(char *d, const char *s) { int i = 0; while (s[i] && i < 63) { d[i] = s[i]; i++; } d[i] = 0; }
static int buf_in_snapshot(const char *buf);    /* fwd: a CoW-snapshot may still hold this buffer (defined below) */
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

/* Positioned read: up to `max` bytes from byte `offset` (M1196). A tmpfs file is
 * a contiguous buffer, so this is a bounded memcpy from buf+offset. */
long tmpfs_pread(const char *name, void *buf, unsigned long max, unsigned long offset) {
    int i = tfind(name);
    if (i < 0) return -1;
    if (tf[i].link) {                         /* symlink: positioned-read the target */
        if (resolve_depth >= 8) return -1;
        resolve_depth++;
        long r = vfs_pread(tf[i].buf, buf, max, offset);
        resolve_depth--;
        return r;
    }
    if (offset >= tf[i].len) return 0;        /* at/after EOF */
    unsigned long n = tf[i].len - offset; if (n > max) n = max;
    for (unsigned long k = 0; k < n; k++) ((char *)buf)[k] = tf[i].buf[offset + k];
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
    if (tf[i].used && tf[i].buf && !buf_in_snapshot(tf[i].buf)) kfree(tf[i].buf);
    tf[i].buf = nb; tf[i].len = (uint32_t)tl; tf[i].link = 1; tf[i].used = 1; tf[i].mtime = rtc_unix(); tcpy(tf[i].name, name);
    return 0;
}

/* readlink (M1233): copy a symlink's TARGET path into buf (NOT followed, NOT
 * NUL-terminated — POSIX). Returns the byte count, or -1 if absent / not a link. */
long tmpfs_readlink(const char *name, void *buf, unsigned long max) {
    int i = tfind(name);
    if (i < 0 || !tf[i].used || !tf[i].link) return -1;
    unsigned long n = tf[i].len; if (n > max) n = max;
    for (unsigned long k = 0; k < n; k++) ((char *)buf)[k] = tf[i].buf[k];
    return (long)n;
}

/* Fill metadata for `name` (statx, M1173): islink, size, mtime. Returns 0/-1. */
int tmpfs_stat(const char *name, int *islink, unsigned long *size, unsigned long *mtime) {
    int i = tfind(name);
    if (i < 0) return -1;
    if (islink) *islink = tf[i].link;
    if (size)   *size = tf[i].len;
    if (mtime)  *mtime = tf[i].mtime;
    return 0;
}

long tmpfs_write(const char *name, const void *data, unsigned long len) {
    int i = tfind(name);
    if (i < 0) { for (i = 0; i < TMPFS_MAX; i++) if (!tf[i].used) break; if (i == TMPFS_MAX) return -1; }
    char *nb = (char *)kmalloc(len ? len : 1);    /* a fresh buffer (write replaces the whole file) */
    if (!nb) return -1;
    for (unsigned long k = 0; k < len; k++) nb[k] = ((const char *)data)[k];
    if (tf[i].used && tf[i].buf && !buf_in_snapshot(tf[i].buf)) kfree(tf[i].buf);
    tf[i].buf = nb; tf[i].len = (uint32_t)len; tf[i].used = 1; tf[i].link = 0; tf[i].mtime = rtc_unix(); tcpy(tf[i].name, name);
    return (long)len;
}

/* truncate a tmpfs file to `newlen` (M1228): a fresh buffer keeping min(old,new)
 * bytes, zero-filled when growing. 0/-1. */
long tmpfs_truncate(const char *name, unsigned long newlen) {
    int i = tfind(name);
    if (i < 0 || !tf[i].used || tf[i].link) return -1;
    char *nb = (char *)kmalloc(newlen ? newlen : 1);
    if (!nb) return -1;
    unsigned long keep = tf[i].len < newlen ? tf[i].len : newlen;
    for (unsigned long k = 0; k < keep; k++)   nb[k] = tf[i].buf[k];
    for (unsigned long k = keep; k < newlen; k++) nb[k] = 0;        /* zero-fill grow */
    if (tf[i].buf && !buf_in_snapshot(tf[i].buf)) kfree(tf[i].buf);
    tf[i].buf = nb; tf[i].len = (uint32_t)newlen; tf[i].mtime = rtc_unix();
    return 0;
}

/* utimensat backend (M1230): set the file's mtime to a Unix epoch (negative =
 * leave unchanged). tmpfs tracks only mtime, so atime is accepted but ignored. */
long tmpfs_utimes(const char *name, long atime, long mtime) {
    (void)atime;
    int i = tfind(name);
    if (i < 0 || !tf[i].used) return -1;
    if (mtime >= 0) tf[i].mtime = (uint32_t)mtime;
    return 0;
}

long tmpfs_remove(const char *name) {
    int i = tfind(name);
    if (i < 0) return -1;
    if (tf[i].buf && !buf_in_snapshot(tf[i].buf)) kfree(tf[i].buf);   /* keep it if a snapshot holds it */
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

/* ---- copy-on-write snapshots (the Plan 9 "dump" filesystem, in miniature) ----
 *
 * A snapshot captures the current set of (name, buffer, len) by RETAINING each
 * live buffer pointer — no data copy. Because every tmpfs_write already allocates
 * a fresh buffer and frees the old one, copy-on-write falls out almost for free:
 * we just must NOT free a buffer a snapshot still holds. So the three free sites
 * are guarded by buf_in_snapshot(), and a write to a snapshotted file leaves the
 * snapshot pointing at the original bytes while the live file moves on.
 *
 * snap_count==0 is the fast path: buf_in_snapshot() returns 0 immediately, so
 * with no snapshots tmpfs behaves byte-for-byte as before (zero blast radius). */
#define SNAP_MAX 4
static struct { struct { char name[64]; char *buf; uint32_t len; int link; } f[TMPFS_MAX]; int n; int used; } snap[SNAP_MAX];
static int snap_count;

static int buf_in_snapshot(const char *buf) {
    if (snap_count == 0 || !buf) return 0;
    for (int s = 0; s < SNAP_MAX; s++) if (snap[s].used)
        for (int j = 0; j < snap[s].n; j++) if (snap[s].f[j].buf == buf) return 1;
    return 0;
}
static int buf_in_tf(const char *buf) {
    if (!buf) return 0;
    for (int i = 0; i < TMPFS_MAX; i++) if (tf[i].used && tf[i].buf == buf) return 1;
    return 0;
}

/* Snapshot the current tmpfs into a free generation slot. Returns the gen index, or -1. */
long tmpfs_snap_create(void) {
    int s; for (s = 0; s < SNAP_MAX; s++) if (!snap[s].used) break;
    if (s == SNAP_MAX) return -1;
    int n = 0;
    for (int i = 0; i < TMPFS_MAX; i++) if (tf[i].used) {
        tcpy(snap[s].f[n].name, tf[i].name);
        snap[s].f[n].buf  = tf[i].buf;          /* RETAIN the live buffer (CoW share) */
        snap[s].f[n].len  = tf[i].len;
        snap[s].f[n].link = tf[i].link;
        n++;
    }
    snap[s].n = n; snap[s].used = 1; snap_count++;
    return s;
}

/* Drop a snapshot, freeing each retained buffer that no live file or other
 * snapshot still references. */
long tmpfs_snap_drop(int s) {
    if (s < 0 || s >= SNAP_MAX || !snap[s].used) return -1;
    snap[s].used = 0;                            /* clear first so buf_in_snapshot ignores us */
    for (int j = 0; j < snap[s].n; j++) {
        char *b = snap[s].f[j].buf; snap[s].f[j].buf = 0;
        if (b && !buf_in_snapshot(b) && !buf_in_tf(b)) kfree(b);
    }
    snap[s].n = 0; snap_count--;
    return 0;
}

/* Read a file as it existed in snapshot `s` (raw bytes — a frozen view does not
 * follow a live symlink). */
long tmpfs_snap_read(int s, const char *name, void *buf, unsigned long max) {
    if (s < 0 || s >= SNAP_MAX || !snap[s].used) return -1;
    for (int j = 0; j < snap[s].n; j++) if (teq(snap[s].f[j].name, name)) {
        unsigned long n = snap[s].f[j].len; if (n > max) n = max;
        for (unsigned long k = 0; k < n; k++) ((char *)buf)[k] = snap[s].f[j].buf[k];
        return (long)n;
    }
    return -1;
}

int tmpfs_snap_list(char *out, int max) {
    int p = 0;
    const char *hdr = "  GEN  FILES\n";
    for (int i = 0; hdr[i] && p < max - 1; i++) out[p++] = hdr[i];
    int any = 0;
    for (int s = 0; s < SNAP_MAX; s++) if (snap[s].used) {
        if (p < max - 1) out[p++] = ' '; if (p < max - 1) out[p++] = ' ';
        if (p < max - 1) out[p++] = (char)('0' + s);
        const char *sep = "    "; for (int i = 0; sep[i] && p < max - 1; i++) out[p++] = sep[i];
        char t[12]; int ti = 0; int v = snap[s].n; if (!v) t[ti++] = '0'; while (v) { t[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti && p < max - 1) out[p++] = t[--ti];
        if (p < max - 1) out[p++] = '\n';
        any = 1;
    }
    if (!any) { const char *m = "  (none — echo create > /snap/ctl)\n"; for (int i = 0; m[i] && p < max - 1; i++) out[p++] = m[i]; }
    if (p < max) out[p] = 0;
    return p;
}

/* /snap/ctl: "create" makes a snapshot; "drop <n>" releases one. */
long tmpfs_snap_control(const void *data, unsigned long len) {
    const char *s = (const char *)data;
    if (len >= 6 && s[0] == 'c' && s[1] == 'r') return tmpfs_snap_create() < 0 ? -1 : (long)len;
    if (len >= 4 && s[0] == 'd' && s[1] == 'r') {
        const char *p = s + 4; while (*p == ' ' && (unsigned long)(p - s) < len) p++;
        int g = 0; int got = 0; while (*p >= '0' && *p <= '9') { g = g * 10 + (*p - '0'); p++; got = 1; }
        if (!got) return -1;
        return tmpfs_snap_drop(g) < 0 ? -1 : (long)len;
    }
    return -1;
}
