/*
 * sysvipc.c — System V semaphores (M1159): keyed counting-semaphore SETS with
 * the classic atomic all-or-nothing semop. semget(key,nsems,flags) opens/creates
 * a set (IPC_PRIVATE always creates; else found by key or created with IPC_CREAT);
 * semop applies an ARRAY of {sem_num, sem_op, sem_flg} ops — but only if EVERY op
 * can proceed without a semaphore going negative (or, for op==0, without a
 * non-zero value); otherwise it blocks the whole call (no partial apply) until
 * woken, unless IPC_NOWAIT. semctl does SETVAL/GETVAL/IPC_RMID. Built on the same
 * IRQ-safe task_block/task_wake rendezvous mqueue.c/mbox.c use. Distinct from the
 * POSIX shm/mqueue/futex the OS already has — this is the keyed SysV API.
 */
#include "sysvipc.h"
#include "task.h"
#include <stdint.h>

#define SEM_N        16   /* max semaphore sets */
#define SEM_PER      16   /* max semaphores per set */
#define SEM_WAITERS  8    /* max tasks blocked on one set */

struct sem_set {
    int     used, key, nsems;
    int     val[SEM_PER];
    task_t *waiters[SEM_WAITERS];
    int     nwait;
};
static struct sem_set sets[SEM_N];

/* Open or create a semaphore set. IPC_PRIVATE (key 0) always makes a fresh set;
 * a positive key reuses an existing set or (with IPC_CREAT) makes one. Returns
 * the set id, or -1. */
int sysv_semget(int key, int nsems, int flags) {
    if (nsems <= 0 || nsems > SEM_PER) return -1;
    if (key != IPC_PRIVATE)
        for (int i = 0; i < SEM_N; i++) if (sets[i].used && sets[i].key == key) return i;
    if (key != IPC_PRIVATE && !(flags & IPC_CREAT)) return -1;   /* not found, not asked to create */
    for (int i = 0; i < SEM_N; i++) if (!sets[i].used) {
        sets[i].used = 1; sets[i].key = key; sets[i].nsems = nsems; sets[i].nwait = 0;
        for (int j = 0; j < nsems; j++) sets[i].val[j] = 0;
        return i;
    }
    return -1;   /* table full */
}

static void wake_all(struct sem_set *s) {      /* a value changed: let every blocked op recheck */
    int n = s->nwait; s->nwait = 0;
    for (int i = 0; i < n; i++) if (s->waiters[i]) task_wake(s->waiters[i]);
}

/* Atomic all-or-nothing semop over an array of ops. Returns 0, or -1 on a bad
 * id/arg or (with IPC_NOWAIT) when it would block. */
int sysv_semop(int id, struct sembuf *sops, unsigned nsops) {
    if (id < 0 || id >= SEM_N || !sets[id].used) return -1;
    if (nsops == 0 || nsops > SEM_PER) return -1;
    struct sem_set *s = &sets[id];
    for (unsigned k = 0; k < nsops; k++)        /* validate sem_num up front */
        if (sops[k].sem_num < 0 || sops[k].sem_num >= s->nsems) return -1;
    for (;;) {
        int would_block = 0, nowait = 0;
        for (unsigned k = 0; k < nsops; k++) {
            int v = s->val[sops[k].sem_num], op = sops[k].sem_op;
            if ((op < 0 && v + op < 0) || (op == 0 && v != 0)) {
                would_block = 1;
                if (sops[k].sem_flg & IPC_NOWAIT) nowait = 1;
            }
        }
        if (!would_block) break;                /* every op can proceed */
        if (nowait) return -1;                  /* EAGAIN: do not apply anything */
        if (s->nwait < SEM_WAITERS) s->waiters[s->nwait++] = task_self();
        task_block();                           /* woken by semop/semctl changing the set */
        if (!s->used) return -1;                /* removed (IPC_RMID) while we blocked */
    }
    for (unsigned k = 0; k < nsops; k++)        /* commit all ops */
        s->val[sops[k].sem_num] += sops[k].sem_op;
    wake_all(s);                                /* increments may unblock other waiters */
    return 0;
}

/* SETVAL (set a semaphore's value), GETVAL (read one), IPC_RMID (remove the set). */
int sysv_semctl(int id, int semnum, int cmd, int arg) {
    if (id < 0 || id >= SEM_N || !sets[id].used) return -1;
    struct sem_set *s = &sets[id];
    if (cmd == IPC_RMID) { s->used = 0; wake_all(s); return 0; }
    if (semnum < 0 || semnum >= s->nsems) return -1;
    if (cmd == GETVAL) return s->val[semnum];
    if (cmd == SETVAL) { s->val[semnum] = arg; wake_all(s); return 0; }
    return -1;
}

/* /proc/sysvipc: one line per live set — "id key nsems val0,val1,...". */
int sysv_sem_format(char *out, int max) {
    int p = 0;
    for (int i = 0; i < SEM_N && p < max - 96; i++) if (sets[i].used) {
        char t[16]; int n;
        for (int pass = 0; pass < 2; pass++) {    /* id, key */
            int v = pass ? sets[i].key : i; n = 0;
            if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
            while (n) out[p++] = t[--n];
            out[p++] = ' ';
        }
        for (int j = 0; j < sets[i].nsems && p < max - 16; j++) {
            int v = sets[i].val[j]; n = 0;
            if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
            while (n) out[p++] = t[--n];
            out[p++] = (j + 1 < sets[i].nsems) ? ',' : '\n';
        }
        if (sets[i].nsems == 0) out[p++] = '\n';
    }
    out[p] = 0; return p;
}
