/*
 * eventfd.c — /timer/<ms> sleepable files and /event/<name> counting eventfds.
 * See eventfd.h. The /event half is a close cousin of notify.c (one blocked
 * reader, woken by a writer, interrupts-off check-and-block) but accumulates a
 * COUNT (write adds, read drains) rather than OR-ing a bitmask.
 */
#include "eventfd.h"
#include "task.h"

/* ----- /timer/<ms> : a sleepable file ----------------------------------- */

long timer_read(const char *ms, void *buf, unsigned long max) {
    if (!ms || max < 6) return -1;
    uint64_t v = 0;
    for (const char *s = ms; *s >= '0' && *s <= '9'; s++) v = v * 10 + (uint64_t)(*s - '0');
    if (v > 600000) v = 600000;               /* cap at 10 minutes */
    __asm__ volatile("sti");                  /* the timer wheel drives the wait (like SYS_sleep) */
    if (v) task_sleep_ms(v);
    const char *t = "tick\n";
    int p = 0; while (t[p] && (unsigned long)p < max - 1) { ((char *)buf)[p] = t[p]; p++; }
    return p;
}

/* ----- /event/<name> : a counting semaphore ----------------------------- */

#define EVENT_N 8

struct event {
    char     name[32];
    int      used;
    uint64_t count;                           /* accumulated, drained on read */
    task_t  *waiter;                          /* one blocked reader */
};
static struct event ev[EVENT_N];

static int eeq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static struct event *event_get(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < EVENT_N; i++) if (ev[i].used && eeq(ev[i].name, name)) return &ev[i];
    for (int i = 0; i < EVENT_N; i++) if (!ev[i].used) {
        int j = 0; while (name[j] && j < 31) { ev[i].name[j] = name[j]; j++; } ev[i].name[j] = 0;
        ev[i].used = 1; ev[i].count = 0; ev[i].waiter = 0;
        return &ev[i];
    }
    return 0;
}

long eventfd_write(const char *name, const void *data, unsigned long len) {
    struct event *e = event_get(name);
    if (!e) return -1;
    uint64_t v = 0; const char *s = (const char *)data;
    for (unsigned long i = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (uint64_t)(s[i] - '0');
    if (v == 0) v = 1;                         /* a bare write counts as one event */
    e->count += v;
    if (e->waiter) { task_wake(e->waiter); e->waiter = 0; }
    return (long)len;
}

long eventfd_read(const char *name, void *buf, unsigned long max) {
    struct event *e = event_get(name);
    if (!e || max < 2) return -1;
    if (e->count == 0) {                       /* nothing pending -> block once */
        e->waiter = task_self();
        task_block();                          /* woken by a writer, a keypress, or a kill */
        e->waiter = 0;
        if (e->count == 0) return 0;           /* stray wake -> don't re-block */
    }
    uint64_t v = e->count; e->count = 0;       /* read-and-drain (atomic: read path is interrupts-off) */
    char t[24]; int ti = 0; uint64_t x = v;
    if (!x) t[ti++] = '0'; else while (x) { t[ti++] = (char)('0' + x % 10); x /= 10; }
    int p = 0;
    while (ti && (unsigned long)p < max - 1) ((char *)buf)[p++] = t[--ti];
    if ((unsigned long)p < max - 1) ((char *)buf)[p++] = '\n';
    return p;
}

static int sapp(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int sdec(char *b, int p, int max, uint64_t v) {
    char t[24]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n]; return p;
}

int eventfd_format(char *b, int max) {
    int p = sapp(b, 0, max, "  EVENT                 COUNT\n");
    int any = 0;
    for (int i = 0; i < EVENT_N; i++) if (ev[i].used) {
        p = sapp(b, p, max, "  "); p = sapp(b, p, max, ev[i].name);
        int nl = 0; while (ev[i].name[nl]) nl++;
        for (int k = nl; k < 20 && p < max - 1; k++) b[p++] = ' ';
        p = sdec(b, p, max, ev[i].count); p = sapp(b, p, max, "\n");
        any = 1;
    }
    if (!any) p = sapp(b, p, max, "  (none — write to /event/<name> to create one)\n");
    if (p < max) b[p] = 0;
    return p;
}
