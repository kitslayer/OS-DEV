/*
 * flock.c — advisory whole-file locks, keyed by path (M1177). See flock.h.
 *
 * A fixed table of held locks {path, owner pid, type}. LOCK_EX conflicts with
 * any other holder; LOCK_SH only with an exclusive holder. A conflicting
 * request without LOCK_NB blocks (task_block) and retries when any unlock wakes
 * it — the IF=0 syscall gate keeps "conflict? then block" atomic against an
 * unlock on this single CPU (the mbox.c discipline). Re-locking a path you
 * already hold upgrades/downgrades it. Locks are advisory: they coordinate
 * cooperating processes, they don't police read/write.
 */
#include "flock.h"
#include "syscall.h"   /* LOCK_SH/EX/NB/UN */
#include "task.h"

#define FLK_N 32                 /* max concurrent held locks */
#define FLK_PATH 64

struct flk { char path[FLK_PATH]; int owner; int type; int used; };  /* type: LOCK_SH / LOCK_EX */
static struct flk fl[FLK_N];
static task_t *fl_waiters[FLK_N];
static int fl_nwait;

static int peq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Would acquiring `want` (LOCK_SH/EX) on `path` for `pid` conflict with a lock
 * held by someone else? EX conflicts with any other holder; SH only with EX. */
static int conflicts(const char *path, int pid, int want) {
    for (int i = 0; i < FLK_N; i++)
        if (fl[i].used && fl[i].owner != pid && peq(fl[i].path, path))
            if (want == LOCK_EX || fl[i].type == LOCK_EX) return 1;
    return 0;
}

static void flk_wake_all(void) {                  /* a lock changed: let every blocked acquirer recheck */
    int n = fl_nwait; fl_nwait = 0;
    for (int i = 0; i < n; i++) if (fl_waiters[i]) task_wake(fl_waiters[i]);
}

int flock_op(const char *path, int pid, int op) {
    if (!path || !path[0]) return -1;
    int nb = op & LOCK_NB, base = op & ~LOCK_NB;

    if (base == LOCK_UN) {                          /* release this pid's lock on the path */
        for (int i = 0; i < FLK_N; i++)
            if (fl[i].used && fl[i].owner == pid && peq(fl[i].path, path)) fl[i].used = 0;
        flk_wake_all();
        return 0;
    }
    if (base != LOCK_SH && base != LOCK_EX) return -1;

    for (;;) {
        if (!conflicts(path, pid, base)) {
            for (int i = 0; i < FLK_N; i++)         /* already hold it -> upgrade/downgrade in place */
                if (fl[i].used && fl[i].owner == pid && peq(fl[i].path, path)) { fl[i].type = base; return 0; }
            for (int i = 0; i < FLK_N; i++) if (!fl[i].used) {   /* take a free slot */
                int j = 0; while (path[j] && j < FLK_PATH - 1) { fl[i].path[j] = path[j]; j++; } fl[i].path[j] = 0;
                fl[i].owner = pid; fl[i].type = base; fl[i].used = 1;
                return 0;
            }
            return -1;                              /* lock table full */
        }
        if (nb) return -1;                          /* EWOULDBLOCK */
        if (fl_nwait < FLK_N) fl_waiters[fl_nwait++] = task_self();
        task_block();                               /* woken by an unlock (or a kill) */
    }
}

void flock_release_pid(int pid) {                   /* process exit: drop everything it held */
    int any = 0;
    for (int i = 0; i < FLK_N; i++) if (fl[i].used && fl[i].owner == pid) { fl[i].used = 0; any = 1; }
    if (any) flk_wake_all();
}

static int sapp(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int sdec(char *b, int p, int max, int v) {
    char t[12]; int n = 0; if (!v) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n];
    return p;
}
int flock_format(char *b, int max) {
    int p = sapp(b, 0, max, "  OWNER  TYPE  PATH\n"), any = 0;
    for (int i = 0; i < FLK_N; i++) if (fl[i].used) {
        p = sapp(b, p, max, "  ");  p = sdec(b, p, max, fl[i].owner);
        p = sapp(b, p, max, fl[i].type == LOCK_EX ? "   EX   " : "   SH   ");
        p = sapp(b, p, max, fl[i].path); p = sapp(b, p, max, "\n"); any = 1;
    }
    if (!any) p = sapp(b, p, max, "  (no locks held)\n");
    if (p < max) b[p] = 0;
    return p;
}
