/*
 * fsevents.c — a circular log of recent filesystem mutations (M1085).
 *
 * kernel/vfs.c calls fsevents_record() after a successful write/delete/mkdir/
 * rename/symlink on a real (tmpfs or FAT32) file — NOT for /proc·/dev, which
 * aren't a filesystem in this sense. procfs serves the log as /proc/fsevents.
 * A fixed ring, byte writes only, safe to call from any FS path.
 */
#include "fsevents.h"
#include "inotify.h"    /* feed real per-fd inotify watches from the same chokepoint (M1266) */
#include <stdint.h>

#define FSEV_N 64                       /* remember the last 64 events */
struct fsev { char op; char path[72]; };
static struct fsev ev[FSEV_N];
static uint32_t fsev_head;              /* total events ever recorded (wraps the ring) */

void fsevents_record(char op, const char *path) {
    if (!path) return;
    int i = fsev_head % FSEV_N;
    ev[i].op = op;
    int j = 0;
    while (path[j] && j < (int)sizeof ev[i].path - 1) { ev[i].path[j] = path[j]; j++; }
    ev[i].path[j] = 0;
    fsev_head++;
    inotify_feed(op, path);                 /* also wake any matching inotify fd watches (M1266) */
}

static const char *op_word(char op) {
    switch (op) {
    case 'w': return "write  ";
    case 'd': return "delete ";
    case 'm': return "mkdir  ";
    case 'r': return "rename ";
    case 'l': return "symlink";
    default:  return "?      ";
    }
}

int fsevents_format(char *b, int max) {
    if (!b || max < 2) return 0;
    uint32_t total = fsev_head;
    uint32_t have = total < FSEV_N ? total : FSEV_N;
    uint32_t start = total - have;          /* oldest event still in the ring */
    int p = 0;
    for (uint32_t k = 0; k < have && p < max - 2; k++) {
        const struct fsev *e = &ev[(start + k) % FSEV_N];
        const char *w = op_word(e->op);
        while (*w && p < max - 1) b[p++] = *w++;
        if (p < max - 1) b[p++] = ' ';
        for (int j = 0; e->path[j] && p < max - 1; j++) b[p++] = e->path[j];
        if (p < max - 1) b[p++] = '\n';
    }
    if (have == 0) { const char *m = "(no filesystem events yet)\n"; while (*m && p < max - 1) b[p++] = *m++; }
    b[p] = 0;
    return p;
}
