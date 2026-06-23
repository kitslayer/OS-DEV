/*
 * mbox.c — named message-queue IPC (M1087). See mbox.h.
 *
 * A fixed table of named queues, each a small ring of bounded messages, created
 * on first use. The VFS routes /ipc/<name> here (kernel/vfs.c): write enqueues,
 * read dequeues FIFO and blocks the caller (task_block) when empty until a
 * writer task_wake()s it. Read-side syscalls run with interrupts off (interrupt
 * gate, no sti on the readfile path), so the "queue empty? then block" check and
 * the block are atomic against a writer on this single CPU — no lost wakeup.
 * A read woken with the queue still empty (a stray keypress wake, or a kill)
 * returns 0 rather than re-blocking, so a `cat /ipc/<empty>` is never stuck.
 */
#include "mbox.h"
#include "task.h"

#define MBOX_N      8           /* up to 8 named queues */
#define MBOX_MSGS   16          /* ring depth per queue */
#define MBOX_MSGSZ  128         /* max message size */

struct mbox {
    char    name[32];
    int     used;
    int     head, tail;         /* ring: empty when head==tail, full when (head+1)%N==tail */
    struct { char data[MBOX_MSGSZ]; int len; } msg[MBOX_MSGS];
    task_t *waiter;             /* a reader blocked on this queue (one consumer) */
};
static struct mbox mb[MBOX_N];

static int meq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Find queue `name`, creating it on first use; NULL if the table is full. */
static struct mbox *mbox_get(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < MBOX_N; i++) if (mb[i].used && meq(mb[i].name, name)) return &mb[i];
    for (int i = 0; i < MBOX_N; i++) if (!mb[i].used) {
        int j = 0; while (name[j] && j < 31) { mb[i].name[j] = name[j]; j++; } mb[i].name[j] = 0;
        mb[i].used = 1; mb[i].head = mb[i].tail = 0; mb[i].waiter = 0;
        return &mb[i];
    }
    return 0;
}

long mbox_write(const char *name, const void *data, unsigned long len) {
    struct mbox *q = mbox_get(name);
    if (!q) return -1;
    int next = (q->head + 1) % MBOX_MSGS;
    if (next == q->tail) return -1;                 /* full */
    if (len > MBOX_MSGSZ) len = MBOX_MSGSZ;
    for (unsigned long i = 0; i < len; i++) q->msg[q->head].data[i] = ((const char *)data)[i];
    q->msg[q->head].len = (int)len;
    q->head = next;
    if (q->waiter) { task_wake(q->waiter); q->waiter = 0; }   /* wake a blocked consumer */
    return (long)len;
}

long mbox_read(const char *name, void *buf, unsigned long max) {
    struct mbox *q = mbox_get(name);
    if (!q) return -1;
    if (q->tail == q->head) {                       /* empty -> block once for a producer */
        q->waiter = task_self();
        task_block();                               /* woken by a writer, a keypress, or a kill */
        q->waiter = 0;
        if (q->tail == q->head) return 0;           /* still empty -> don't re-block */
    }
    int n = q->msg[q->tail].len;
    if ((unsigned long)n > max) n = (int)max;
    for (int i = 0; i < n; i++) ((char *)buf)[i] = q->msg[q->tail].data[i];
    q->tail = (q->tail + 1) % MBOX_MSGS;
    return n;
}

static int sapp(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int sdec(char *b, int p, int max, int v) {
    char t[12]; int n = 0; if (!v) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n]; return p;
}

int mbox_format(char *b, int max) {
    int p = sapp(b, 0, max, "  QUEUE                 PENDING\n");
    int any = 0;
    for (int i = 0; i < MBOX_N; i++) if (mb[i].used) {
        int depth = (mb[i].head - mb[i].tail + MBOX_MSGS) % MBOX_MSGS;
        p = sapp(b, p, max, "  "); p = sapp(b, p, max, mb[i].name);
        int pad = 20 - (int)0; const char *nm = mb[i].name; int nl = 0; while (nm[nl]) nl++;
        for (int k = nl; k < pad && p < max - 1; k++) b[p++] = ' ';
        p = sdec(b, p, max, depth); p = sapp(b, p, max, "\n");
        any = 1;
    }
    if (!any) p = sapp(b, p, max, "  (no mailboxes — write to /ipc/<name> to create one)\n");
    if (p < max) b[p] = 0;
    return p;
}
