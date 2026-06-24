/*
 * unixsock.c — path-keyed AF_UNIX stream sockets (M1169). See unixsock.h.
 *
 * A fixed table of connections, each a pair of byte rings (a2b, b2a) so the two
 * endpoints stream in both directions, plus a fixed table of listeners keyed by
 * pathname. An ENDPOINT id packs the connection index and which side (A=client,
 * B=server): ep = (conn<<1)|side. send writes the side's TX ring (= the peer's
 * RX ring) and wakes the peer; recv reads the side's RX ring and — mirroring
 * mbox.c — blocks once when empty (relying on the IF=0 int-0x80 syscall gate, so
 * "empty? then block" is atomic against a sender on this single CPU: a sender
 * only runs after we've recorded our waiter and called task_block), returning 0
 * at EOF (peer closed and the ring is drained). No fd table is needed: the small
 * integer ep handle is the socket reference, and because it indexes this global
 * table it stays valid across fork().
 */
#include "unixsock.h"
#include "task.h"

#define U_LISTEN  8               /* concurrent listeners */
#define U_CONN    16              /* concurrent connections (each = 2 endpoints) */
#define U_RING    4096            /* bytes buffered per direction */
#define U_PATH    64

struct uring { unsigned char buf[U_RING]; int head, tail; };   /* empty when head==tail */
static int rcount(struct uring *r) { return (r->head - r->tail + U_RING) % U_RING; }
static int rfree(struct uring *r)  { return U_RING - 1 - rcount(r); }   /* keep one slot empty to tell full from empty */
static int rput(struct uring *r, const unsigned char *d, int n) {
    int f = rfree(r); if (n > f) n = f;
    for (int i = 0; i < n; i++) { r->buf[r->head] = d[i]; r->head = (r->head + 1) % U_RING; }
    return n;
}
static int rget(struct uring *r, unsigned char *d, int n) {
    int c = rcount(r); if (n > c) n = c;
    for (int i = 0; i < n; i++) { d[i] = r->buf[r->tail]; r->tail = (r->tail + 1) % U_RING; }
    return n;
}

struct uconn {
    int used;
    struct uring a2b, b2a;        /* A(client)->B(server) stream, and B->A stream */
    int a_closed, b_closed;
    task_t *a_waiter, *b_waiter;  /* side A blocked reading b2a; side B blocked reading a2b */
};
static struct uconn conns[U_CONN];

struct ulisten {
    int used;
    char path[U_PATH];
    int  pend[U_CONN];            /* connection indices awaiting accept (FIFO) */
    int  np;
    task_t *waiter;               /* a server blocked in accept */
};
static struct ulisten lis[U_LISTEN];

static int peq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int unix_listen(const char *path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < U_LISTEN; i++) if (lis[i].used && peq(lis[i].path, path)) return i;   /* already bound: idempotent re-listen (single-user OS) */
    for (int i = 0; i < U_LISTEN; i++) if (!lis[i].used) {
        int j = 0; while (path[j] && j < U_PATH - 1) { lis[i].path[j] = path[j]; j++; } lis[i].path[j] = 0;
        lis[i].used = 1; lis[i].np = 0; lis[i].waiter = 0;
        return i;
    }
    return -1;                                    /* listener table full */
}

int unix_connect(const char *path) {
    if (!path) return -1;
    int li = -1; for (int i = 0; i < U_LISTEN; i++) if (lis[i].used && peq(lis[i].path, path)) { li = i; break; }
    if (li < 0) return -1;                         /* nobody listening on this path */
    if (lis[li].np >= U_CONN) return -1;           /* accept backlog full */
    int ci = -1; for (int i = 0; i < U_CONN; i++) if (!conns[i].used) { ci = i; break; }
    if (ci < 0) return -1;                         /* connection table full */
    struct uconn *c = &conns[ci];
    c->used = 1; c->a2b.head = c->a2b.tail = 0; c->b2a.head = c->b2a.tail = 0;
    c->a_closed = c->b_closed = 0; c->a_waiter = c->b_waiter = 0;
    lis[li].pend[lis[li].np++] = ci;               /* enqueue for the server to accept */
    if (lis[li].waiter) { task_wake(lis[li].waiter); lis[li].waiter = 0; }
    return (ci << 1) | 0;                          /* client gets side A */
}

int unix_accept(int lid) {
    if (lid < 0 || lid >= U_LISTEN || !lis[lid].used) return -1;
    struct ulisten *l = &lis[lid];
    if (l->np == 0) {                              /* no pending connection -> block for one */
        l->waiter = task_self();
        task_block();                              /* woken by a connector (or a kill) */
        l->waiter = 0;
        if (l->np == 0) return -1;                 /* spurious wake -> nothing to accept */
    }
    int ci = l->pend[0];                           /* FIFO dequeue */
    for (int i = 1; i < l->np; i++) l->pend[i - 1] = l->pend[i];
    l->np--;
    return (ci << 1) | 1;                          /* server gets side B */
}

/* Resolve an endpoint id to its connection + side (0=A, 1=B); 0 if invalid. */
static struct uconn *ep_conn(int ep, int *side) {
    if (ep < 0) return 0;
    int ci = ep >> 1;
    if (ci >= U_CONN || !conns[ci].used) return 0;
    *side = ep & 1;
    return &conns[ci];
}

long unix_send(int ep, const void *buf, unsigned long len) {
    int s; struct uconn *c = ep_conn(ep, &s); if (!c) return -1;
    int peer_closed = s ? c->a_closed : c->b_closed;
    if (peer_closed) return -1;                                  /* peer is gone */
    struct uring *tx = s ? &c->b2a : &c->a2b;                    /* B writes b2a, A writes a2b */
    int n = rput(tx, (const unsigned char *)buf, (int)len);
    task_t **pw = s ? &c->a_waiter : &c->b_waiter;               /* wake the peer's blocked reader */
    if (n > 0 && *pw) { task_wake(*pw); *pw = 0; }
    return n;
}

long unix_recv(int ep, void *buf, unsigned long max) {
    int s; struct uconn *c = ep_conn(ep, &s); if (!c) return -1;
    struct uring *rx = s ? &c->a2b : &c->b2a;                    /* B reads a2b, A reads b2a */
    int self_closed = s ? c->b_closed : c->a_closed;
    if (self_closed) return -1;                                  /* we closed our own end */
    if (rcount(rx) == 0) {
        int peer_closed = s ? c->a_closed : c->b_closed;
        if (peer_closed) return 0;                               /* EOF: peer closed and ring drained */
        task_t **mw = s ? &c->b_waiter : &c->a_waiter;
        *mw = task_self();
        task_block();                                            /* woken by the peer's send/close (or a kill) */
        *mw = 0;
        if (rcount(rx) == 0) return 0;                           /* still empty -> EOF / spurious, don't re-block */
    }
    return rget(rx, (unsigned char *)buf, (int)max);
}

int unix_close(int ep) {
    int s; struct uconn *c = ep_conn(ep, &s); if (!c) return -1;
    if (s) c->b_closed = 1; else c->a_closed = 1;
    task_t **pw = s ? &c->a_waiter : &c->b_waiter;               /* wake the peer so its recv returns EOF */
    if (*pw) { task_wake(*pw); *pw = 0; }
    if (c->a_closed && c->b_closed) c->used = 0;                 /* both ends gone -> free the slot */
    return 0;
}

static int sapp(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int sdec(char *b, int p, int max, int v) {
    char t[12]; int n = 0; if (!v) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n];
    return p;
}
int unix_format(char *b, int max) {
    int p = sapp(b, 0, max, "LISTENERS\n"), any = 0;
    for (int i = 0; i < U_LISTEN; i++) if (lis[i].used) {
        p = sapp(b, p, max, "  "); p = sapp(b, p, max, lis[i].path);
        p = sapp(b, p, max, "  backlog="); p = sdec(b, p, max, lis[i].np); p = sapp(b, p, max, "\n"); any = 1;
    }
    if (!any) p = sapp(b, p, max, "  (none)\n");
    p = sapp(b, p, max, "CONNECTIONS\n"); any = 0;
    for (int i = 0; i < U_CONN; i++) if (conns[i].used) {
        p = sapp(b, p, max, "  conn"); p = sdec(b, p, max, i);
        p = sapp(b, p, max, ": a2b="); p = sdec(b, p, max, rcount(&conns[i].a2b));
        p = sapp(b, p, max, "B b2a="); p = sdec(b, p, max, rcount(&conns[i].b2a)); p = sapp(b, p, max, "B");
        if (conns[i].a_closed) p = sapp(b, p, max, " A-closed");
        if (conns[i].b_closed) p = sapp(b, p, max, " B-closed");
        p = sapp(b, p, max, "\n"); any = 1;
    }
    if (!any) p = sapp(b, p, max, "  (none)\n");
    if (p < max) b[p] = 0;
    return p;
}
