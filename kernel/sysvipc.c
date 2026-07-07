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
#include "app.h"        /* app_shm_open (SysV shm reuses the M1108 named-shm backing) */
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

/* ---- System V message queues (M1160) ---------------------------------------
 * Keyed queues of TYPED messages. msgsnd appends a message tagged with a
 * positive mtype; msgrcv selects by `mtyp`: 0 = the oldest message, >0 = the
 * oldest with mtype == mtyp, <0 = the oldest among messages with mtype <= |mtyp|
 * (picking the LOWEST such mtype first). Blocks (task_block) when full/empty.
 * Distinct from the M1154 POSIX mqueue (priority-ordered, named): this is the
 * keyed, type-selected SysV API. */
#define MSG_N    8
#define MSG_MAX  32
#define MSG_SZ   192
struct sysv_msg { int used; long mtype; uint32_t seq; int len; char data[MSG_SZ]; };
struct msg_q {
    int used, key, count; uint32_t seq;
    struct sysv_msg m[MSG_MAX];
    task_t *swait[4], *rwait[4]; int nsw, nrw;
};
static struct msg_q mqs[MSG_N];

static void mq_wake(task_t **w, int *n) { int k = *n; *n = 0; for (int i = 0; i < k; i++) if (w[i]) task_wake(w[i]); }

int sysv_msgget(int key, int flags) {
    if (key != IPC_PRIVATE) for (int i = 0; i < MSG_N; i++) if (mqs[i].used && mqs[i].key == key) return i;
    if (key != IPC_PRIVATE && !(flags & IPC_CREAT)) return -1;
    for (int i = 0; i < MSG_N; i++) if (!mqs[i].used) {
        mqs[i].used = 1; mqs[i].key = key; mqs[i].count = 0; mqs[i].seq = 0; mqs[i].nsw = mqs[i].nrw = 0;
        for (int j = 0; j < MSG_MAX; j++) mqs[i].m[j].used = 0;
        return i;
    }
    return -1;
}

int sysv_msgsnd(int id, long mtype, const void *data, int len, int flags) {
    if (id < 0 || id >= MSG_N || !mqs[id].used || mtype <= 0) return -1;
    if (len < 0 || len > MSG_SZ) return -1;
    struct msg_q *q = &mqs[id];
    while (q->count >= MSG_MAX) {                       /* full -> block (unless IPC_NOWAIT) */
        if (flags & IPC_NOWAIT) return -1;
        if (q->nsw < 4) q->swait[q->nsw++] = task_self();
        task_block();
        if (!q->used) return -1;
    }
    for (int i = 0; i < MSG_MAX; i++) if (!q->m[i].used) {
        q->m[i].used = 1; q->m[i].mtype = mtype; q->m[i].seq = q->seq++; q->m[i].len = len;
        for (int b = 0; b < len; b++) q->m[i].data[b] = ((const char *)data)[b];
        q->count++;
        mq_wake(q->rwait, &q->nrw);
        return 0;
    }
    return -1;
}

int sysv_msgrcv(int id, long mtyp, void *out, int max, long *mtype_out, int flags) {
    if (id < 0 || id >= MSG_N || !mqs[id].used) return -1;
    struct msg_q *q = &mqs[id];
    for (;;) {
        int best = -1;
        for (int i = 0; i < MSG_MAX; i++) if (q->m[i].used) {
            long t = q->m[i].mtype;
            int match = (mtyp == 0) || (mtyp > 0 && t == mtyp) || (mtyp < 0 && t <= -mtyp);
            if (!match) continue;
            if (best < 0) { best = i; continue; }
            if (mtyp < 0) {            /* lowest mtype, then oldest */
                if (t < q->m[best].mtype || (t == q->m[best].mtype && q->m[i].seq < q->m[best].seq)) best = i;
            } else if (q->m[i].seq < q->m[best].seq) best = i;   /* oldest match */
        }
        if (best >= 0) {
            int n = q->m[best].len; if (n > max) n = max;
            for (int b = 0; b < n; b++) ((char *)out)[b] = q->m[best].data[b];
            if (mtype_out) *mtype_out = q->m[best].mtype;
            q->m[best].used = 0; q->count--;
            mq_wake(q->swait, &q->nsw);
            return n;
        }
        if (flags & IPC_NOWAIT) return -1;             /* no match -> block (unless IPC_NOWAIT) */
        if (q->nrw < 4) q->rwait[q->nrw++] = task_self();
        task_block();
        if (!q->used) return -1;
    }
}

/* IPC_RMID (M1576): the removal path this table never had. Before this,
 * MSG_N=8 message queues, ever created, permanently exhausted the table for
 * the machine's uptime -- mirrors sysv_semctl's own IPC_RMID exactly, waking
 * every blocked sender/receiver so nobody is left parked on a removed queue. */
int sysv_msgctl(int id, int cmd) {
    if (id < 0 || id >= MSG_N || !mqs[id].used) return -1;
    if (cmd != IPC_RMID) return -1;
    mqs[id].used = 0;
    mq_wake(mqs[id].swait, &mqs[id].nsw);
    mq_wake(mqs[id].rwait, &mqs[id].nrw);
    return 0;
}

/* ---- System V shared memory (M1161) ----------------------------------------
 * Keyed shared-memory segments. shmget(key,size,flags) opens/creates a segment;
 * shmat maps it into the caller's address space; shmdt unmaps. A segment is just
 * a named object backed by the existing M1108 shm machinery (app_shm_open) —
 * each segment gets a stable synthetic name, so two shmat()s of the same id (in
 * the same or different processes) map the SAME physical frames. */
#define SHM_N      16
#define SHM_MAXSZ  (4u << 20)   /* 4 MiB cap per segment */
struct shm_seg { int used, key; uint64_t size; char name[16]; };
static struct shm_seg shms[SHM_N];

int sysv_shmget(int key, uint64_t size, int flags) {
    if (size == 0 || size > SHM_MAXSZ) return -1;
    if (key != IPC_PRIVATE) for (int i = 0; i < SHM_N; i++) if (shms[i].used && shms[i].key == key) return i;
    if (key != IPC_PRIVATE && !(flags & IPC_CREAT)) return -1;
    for (int i = 0; i < SHM_N; i++) if (!shms[i].used) {
        shms[i].used = 1; shms[i].key = key; shms[i].size = size;
        char *n = shms[i].name; int p = 0;                 /* stable synthetic name "sysvshmNN" */
        const char *pre = "sysvshm"; while (*pre) n[p++] = *pre++;
        n[p++] = (char)('0' + i / 10); n[p++] = (char)('0' + i % 10); n[p] = 0;
        return i;
    }
    return -1;
}

/* Attach: map the segment into the caller's address space, return the base VA
 * (or 0). Two attaches of the same id share the backing (same synthetic name). */
uint64_t sysv_shmat(int id) {
    if (id < 0 || id >= SHM_N || !shms[id].used) return 0;
    return app_shm_open(shms[id].name, shms[id].size);
}

/* IPC_RMID (M1576): frees THIS id slot (SHM_N=16, permanently exhausted
 * without this before now) so a future sysv_shmget can reuse the number --
 * matching real shmctl(IPC_RMID)'s effect on the id namespace. Deliberately
 * scoped no further: the underlying named object in shm.c (its own separate,
 * smaller SHM_N=8 table, keyed by THIS segment's synthetic "sysvshmNN" name,
 * derived deterministically from `id`) has no removal path of its own --
 * shm_open()'d objects have never been freeable here, matching every
 * existing shm_open caller's behavior today. One real, honestly-stated
 * consequence: since the synthetic name is a pure function of `id`, if a
 * freed id is later reused, shm_get's own find-by-name path will find and
 * hand back the SAME (never-actually-cleared) frames rather than a fresh
 * zeroed segment -- IPC_RMID here frees the ID for reuse, it does not (yet)
 * guarantee a truly fresh segment on the next shmget of that same id. */
int sysv_shmctl(int id, int cmd) {
    if (id < 0 || id >= SHM_N || !shms[id].used) return -1;
    if (cmd != IPC_RMID) return -1;
    shms[id].used = 0;
    return 0;
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
