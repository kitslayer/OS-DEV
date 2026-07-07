/*
 * sem.c — POSIX named semaphores (M1575): sem_open/wait/trywait/post/close/
 * unlink/getvalue. Distinct from the keyed SysV semaphore SETS in sysvipc.c
 * (multi-semaphore, atomic multi-op semop) -- this is a single counting
 * semaphore per NAME, shared by any process that sem_opens the same name,
 * with independent per-process handles: sem_close only drops one handle
 * (refcounted), the semaphore itself persists until sem_unlink AND every
 * handle has closed, matching real POSIX exactly. Built on the same
 * IRQ-safe task_block/task_wake rendezvous mqueue.c/sysvipc.c use, and the
 * same wake-everyone-let-them-recheck idiom sysvipc.c's own wake_all uses
 * (safe under this codebase's plain task_block/task_wake model, rather than
 * trying to wake exactly one waiter by queue position).
 */
#include "sem.h"
#include "task.h"
#include <stdint.h>

#define SEM_N       16   /* max named semaphores */
#define SEM_WAITERS 8    /* max tasks blocked on one semaphore */

struct psem {
    char    name[32];
    int     used, unlinked, value, refcount;
    task_t *waiters[SEM_WAITERS];
    int     nwait;
};
static struct psem tab[SEM_N];

static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Open (find-or-create) a named semaphore. O_CREAT|O_EXCL fails (EEXIST) if
 * it already exists; a plain O_CREAT reuses an existing one silently
 * (value/mode ignored on reuse, matching real POSIX). Each successful open
 * -- even a second one from the SAME process -- gets its own refcounted
 * handle that must be independently closed. */
int sem_named_open(const char *name, int oflag, unsigned int value) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < SEM_N; i++)
        if (tab[i].used && !tab[i].unlinked && seq(tab[i].name, name)) {
            if ((oflag & O_CREAT) && (oflag & O_EXCL)) return -1;   /* EEXIST */
            tab[i].refcount++;
            return i;
        }
    if (!(oflag & O_CREAT)) return -1;   /* ENOENT */
    for (int i = 0; i < SEM_N; i++) if (!tab[i].used) {
        int j = 0; while (name[j] && j < 31) { tab[i].name[j] = name[j]; j++; } tab[i].name[j] = 0;
        tab[i].used = 1; tab[i].unlinked = 0; tab[i].value = (int)value; tab[i].refcount = 1; tab[i].nwait = 0;
        return i;
    }
    return -1;   /* table full */
}

int sem_named_close(int idx) {
    if (idx < 0 || idx >= SEM_N || !tab[idx].used) return -1;
    if (tab[idx].refcount > 0) tab[idx].refcount--;
    if (tab[idx].refcount == 0 && tab[idx].unlinked) tab[idx].used = 0;
    return 0;
}

/* Removes the NAME so no new sem_open can find it; existing handles (and
 * anyone still blocked in sem_wait on this index) stay valid until closed. */
int sem_named_unlink(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < SEM_N; i++) if (tab[i].used && !tab[i].unlinked && seq(tab[i].name, name)) {
        tab[i].unlinked = 1;
        if (tab[i].refcount == 0) tab[i].used = 0;   /* nobody holds it open -- free now */
        return 0;
    }
    return -1;   /* ENOENT */
}

static void wake_all(struct psem *s) {   /* a post changed the value: let every blocked waiter recheck */
    int n = s->nwait; s->nwait = 0;
    for (int i = 0; i < n; i++) if (s->waiters[i]) task_wake(s->waiters[i]);
}

int sem_named_wait(int idx) {
    if (idx < 0 || idx >= SEM_N || !tab[idx].used) return -1;
    struct psem *s = &tab[idx];
    while (s->value == 0) {
        if (s->nwait < SEM_WAITERS) s->waiters[s->nwait++] = task_self();
        task_block();
        if (!s->used) return -1;    /* removed (fully closed + unlinked) while blocked */
    }
    s->value--;
    return 0;
}

int sem_named_trywait(int idx) {
    if (idx < 0 || idx >= SEM_N || !tab[idx].used) return -1;
    struct psem *s = &tab[idx];
    if (s->value == 0) return -1;   /* EAGAIN */
    s->value--;
    return 0;
}

int sem_named_post(int idx) {
    if (idx < 0 || idx >= SEM_N || !tab[idx].used) return -1;
    struct psem *s = &tab[idx];
    s->value++;
    wake_all(s);
    return 0;
}

int sem_named_getvalue(int idx, int *out) {
    if (idx < 0 || idx >= SEM_N || !tab[idx].used || !out) return -1;
    *out = tab[idx].value;
    return 0;
}
