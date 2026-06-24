/*
 * mqueue.c — POSIX-style priority message queues (M1154).
 *
 * Named, bounded queues where every message carries a PRIORITY: a receive
 * returns the HIGHEST-priority message (FIFO within a priority), unlike the
 * plain FIFO /ipc mailboxes (mbox.c). Senders block when the queue is full,
 * receivers block when it is empty — the same IRQ-safe task_block/task_wake
 * rendezvous mbox.c uses (task_block itself brackets interrupts). Queues are
 * shared by NAME, so two processes that mq_open the same name share one queue
 * (e.g. a fork parent/child) — no per-process fd table needed. Bounded; never
 * allocates.
 */
#include "mqueue.h"
#include "task.h"
#include <stdint.h>

#define MQ_N       16     /* max simultaneous named queues */
#define MQ_MAXMSG  16     /* max messages held per queue    */
#define MQ_MSGSZ   128    /* max message payload (bytes)     */

struct mqmsg { uint8_t used, prio; uint32_t seq; int len; char data[MQ_MSGSZ]; };
struct mqueue {
    char   name[32];
    int    used, maxmsg, msgsize, count;
    uint32_t seq;                       /* ever-increasing, for FIFO-within-priority ties */
    struct mqmsg msg[MQ_MAXMSG];
    task_t *send_waiter, *recv_waiter;  /* one blocked sender / receiver (mbox-style) */
};
static struct mqueue mq[MQ_N];

static int meq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Open (find-or-create) the named queue; returns its index, or -1 if the table
 * is full / the name is empty. maxmsg/msgsize are clamped to the static caps. */
int mqueue_open(const char *name, int maxmsg, int msgsize) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < MQ_N; i++) if (mq[i].used && meq(mq[i].name, name)) return i;
    for (int i = 0; i < MQ_N; i++) if (!mq[i].used) {
        int j = 0; while (name[j] && j < 31) { mq[i].name[j] = name[j]; j++; } mq[i].name[j] = 0;
        mq[i].used = 1; mq[i].count = 0; mq[i].seq = 0;
        mq[i].maxmsg  = (maxmsg  > 0 && maxmsg  <= MQ_MAXMSG) ? maxmsg  : MQ_MAXMSG;
        mq[i].msgsize = (msgsize > 0 && msgsize <= MQ_MSGSZ)  ? msgsize : MQ_MSGSZ;
        mq[i].send_waiter = mq[i].recv_waiter = 0;
        for (int k = 0; k < MQ_MAXMSG; k++) mq[i].msg[k].used = 0;
        return i;
    }
    return -1;
}

/* Enqueue a message with priority `prio`; blocks while the queue is full. The
 * payload is truncated to msgsize. Returns the bytes stored, or -1. */
long mqueue_send(int idx, const void *buf, unsigned long len, unsigned int prio) {
    if (idx < 0 || idx >= MQ_N || !mq[idx].used) return -1;
    struct mqueue *q = &mq[idx];
    if ((int)len > q->msgsize) len = (unsigned long)q->msgsize;
    while (q->count >= q->maxmsg) {                 /* full -> block for room */
        q->send_waiter = task_self();
        task_block();
        q->send_waiter = 0;
        if (!q->used) return -1;
    }
    for (int i = 0; i < MQ_MAXMSG; i++) if (!q->msg[i].used) {
        q->msg[i].used = 1; q->msg[i].prio = (uint8_t)prio; q->msg[i].seq = q->seq++;
        q->msg[i].len = (int)len;
        for (unsigned long k = 0; k < len; k++) q->msg[i].data[k] = ((const char *)buf)[k];
        q->count++;
        if (q->recv_waiter) { task_wake(q->recv_waiter); q->recv_waiter = 0; }
        return (long)len;
    }
    return -1;
}

/* Dequeue the HIGHEST-priority message (oldest seq among equal priorities);
 * blocks while the queue is empty. Writes the message priority to *prio_out (if
 * non-NULL) and the payload (truncated to `max`) to buf. Returns the byte
 * length, or -1. */
long mqueue_receive(int idx, void *buf, unsigned long max, unsigned int *prio_out) {
    if (idx < 0 || idx >= MQ_N || !mq[idx].used) return -1;
    struct mqueue *q = &mq[idx];
    while (q->count == 0) {                          /* empty -> block for a message */
        q->recv_waiter = task_self();
        task_block();
        q->recv_waiter = 0;
        if (!q->used || q->count == 0) return -1;    /* woken but still empty (e.g. killed) */
    }
    int best = -1;
    for (int i = 0; i < MQ_MAXMSG; i++) if (q->msg[i].used) {
        if (best < 0 || q->msg[i].prio > q->msg[best].prio ||
            (q->msg[i].prio == q->msg[best].prio && q->msg[i].seq < q->msg[best].seq))
            best = i;
    }
    if (best < 0) return -1;
    int n = q->msg[best].len; if ((unsigned long)n > max) n = (int)max;
    for (int k = 0; k < n; k++) ((char *)buf)[k] = q->msg[best].data[k];
    if (prio_out) *prio_out = q->msg[best].prio;
    q->msg[best].used = 0; q->count--;
    if (q->send_waiter) { task_wake(q->send_waiter); q->send_waiter = 0; }
    return (long)n;
}

/* Backs /proc/mqueue: one line per open queue — "name  cur/max  msgsize". */
int mqueue_format(char *out, int max) {
    int p = 0;
    for (int i = 0; i < MQ_N; i++) if (mq[i].used && p < max - 80) {
        for (const char *s = mq[i].name; *s && p < max - 40; s++) out[p++] = *s;
        out[p++] = ' '; out[p++] = ' ';
        char t[12]; int n = 0, v = mq[i].count; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) out[p++] = t[--n];
        out[p++] = '/';
        n = 0; v = mq[i].maxmsg; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) out[p++] = t[--n];
        out[p++] = '\n';
    }
    out[p] = 0; return p;
}
