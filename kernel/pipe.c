/*
 * pipe.c — anonymous pipe objects (M1187). See pipe.h.
 *
 * Each pipe is one ring buffer (writer -> reader) plus r_open/w_open reference
 * counts (how many fds, across all processes, hold the read / write end). A read
 * blocks while the ring is empty and a writer still exists, and returns 0 (EOF)
 * once all writers have closed and the ring is drained. A write blocks while the
 * ring is full and a reader still exists, and returns -1 (EPIPE) once all readers
 * have closed. The slot is freed when both ends reach zero. Mirrors the
 * lost-wakeup-free block/wake discipline of mbox.c / unixsock.c / pty.c.
 */
#include "pipe.h"
#include "task.h"

#define NPIPE 32
#define PBUF  4096

struct kpipe {
    int used;
    unsigned char b[PBUF];
    int head, tail;                  /* ring; empty when head==tail, one slot kept free */
    int r_open, w_open;              /* reference counts on the read / write ends */
    int pinned;                      /* a FIFO's backing pipe: persists at 0/0 (M1188) */
    int had_writer;                  /* a writer has connected at least once (FIFO EOF gating, M1188) */
    task_t *rw, *ww;                 /* a blocked reader / writer */
};
static struct kpipe pipes[NPIPE];

static int p_cnt(struct kpipe *p) { return (p->head - p->tail + PBUF) % PBUF; }
static int p_spc(struct kpipe *p) { return PBUF - 1 - p_cnt(p); }
static struct kpipe *pp(int idx) { return (idx < 0 || idx >= NPIPE || !pipes[idx].used) ? 0 : &pipes[idx]; }

/* Non-destructive readiness (poll/select, M1210). A read won't block iff there
 * is buffered data, OR EOF has been reached (no writers left, and -- for a FIFO
 * -- a writer has connected at least once: matches pipe_read's EOF gating). */
int pipe_readable(int idx) {
    struct kpipe *p = pp(idx); if (!p) return 0;
    return p_cnt(p) > 0 || (p->w_open == 0 && p->had_writer);
}
/* A write won't block iff the ring has room, or no reader remains (a write to a
 * reader-less pipe raises EPIPE and returns at once rather than blocking). */
int pipe_writable(int idx) {
    struct kpipe *p = pp(idx); if (!p) return 0;
    return p->r_open == 0 || p_spc(p) > 0;
}

int pipe_new(void) {
    for (int i = 0; i < NPIPE; i++) if (!pipes[i].used) {
        struct kpipe *p = &pipes[i];
        for (unsigned k = 0; k < sizeof *p; k++) ((unsigned char *)p)[k] = 0;
        p->used = 1; p->r_open = 1; p->w_open = 1; p->had_writer = 1;   /* anon: the creator holds both ends */
        return i;
    }
    return -1;
}

/* A FIFO's backing pipe (M1188): unopened (0/0 — opens are per-process) and
 * `pinned` so it survives at 0/0 (the named pipe outlives any single open). A
 * reader can't EOF until a writer has connected at least once (had_writer). */
int pipe_new_fifo(void) {
    int idx = pipe_new(); if (idx < 0) return -1;
    struct kpipe *p = &pipes[idx];
    p->r_open = 0; p->w_open = 0; p->pinned = 1; p->had_writer = 0;
    return idx;
}

long pipe_read(int idx, void *buf, unsigned long max) {
    struct kpipe *p = pp(idx); if (!p) return -1;
    for (;;) {
        if (p_cnt(p) > 0) {
            unsigned char *d = (unsigned char *)buf; long g = 0;
            while ((unsigned long)g < max && p_cnt(p) > 0) { d[g++] = p->b[p->tail]; p->tail = (p->tail + 1) % PBUF; }
            if (p->ww) { task_wake(p->ww); p->ww = 0; }          /* a blocked writer now has room */
            return g;
        }
        if (p->w_open == 0 && p->had_writer) return 0;           /* EOF: drained + no writers (FIFO: only after one connected) */
        p->rw = task_self(); task_block();                       /* woken by a writer/close (or a kill) */
        p = pp(idx); if (!p) return -1;                          /* freed under us */
    }
}

long pipe_write(int idx, const void *buf, unsigned long len) {
    struct kpipe *p = pp(idx); if (!p) return -1;
    const unsigned char *d = (const unsigned char *)buf;
    unsigned long done = 0;
    while (done < len) {
        if (p->r_open == 0) return done ? (long)done : -1;       /* EPIPE: no readers left */
        if (p_spc(p) > 0) {
            while (done < len && p_spc(p) > 0) { p->b[p->head] = d[done++]; p->head = (p->head + 1) % PBUF; }
            if (p->rw) { task_wake(p->rw); p->rw = 0; }          /* a blocked reader now has data */
            continue;
        }
        p->ww = task_self(); task_block();                       /* ring full: wait for a reader to drain */
        p = pp(idx); if (!p) return done ? (long)done : -1;
    }
    return (long)done;
}

/* Move up to `max` bytes from pipe `in` to pipe `out`, ring-to-ring, with no
 * userspace bounce (splice, M1211). Non-blocking: transfers only what's buffered
 * in `in` and fits in `out` right now. Consumes from `in`. Returns bytes moved
 * (0 = in empty / out full / EOF); -1 on a bad/identical index. */
long pipe_splice(int in, int out, unsigned long max) {
    struct kpipe *pi = pp(in), *po = pp(out); if (!pi || !po || in == out) return -1;
    long n = 0;
    while ((unsigned long)n < max && p_cnt(pi) > 0 && p_spc(po) > 0) {
        po->b[po->head] = pi->b[pi->tail];
        po->head = (po->head + 1) % PBUF;
        pi->tail = (pi->tail + 1) % PBUF;
        n++;
    }
    if (n) {
        if (po->rw) { task_wake(po->rw); po->rw = 0; }           /* dest now has data */
        if (pi->ww) { task_wake(pi->ww); pi->ww = 0; }           /* source now has room */
    }
    return n;
}
/* Copy up to `max` bytes from pipe `in` to pipe `out` WITHOUT consuming `in`
 * (tee, M1211): the source stays fully readable. Non-blocking. Returns bytes
 * copied (0 = nothing buffered / out full); -1 on a bad/identical index. */
long pipe_tee(int in, int out, unsigned long max) {
    struct kpipe *pi = pp(in), *po = pp(out); if (!pi || !po || in == out) return -1;
    long n = 0; int t = pi->tail;
    while ((unsigned long)n < max && t != pi->head && p_spc(po) > 0) {
        po->b[po->head] = pi->b[t];
        po->head = (po->head + 1) % PBUF;
        t = (t + 1) % PBUF;
        n++;
    }
    if (n && po->rw) { task_wake(po->rw); po->rw = 0; }          /* dest now has data */
    return n;
}

void pipe_open_end(int idx, int write_end) {
    struct kpipe *p = pp(idx); if (!p) return;
    if (write_end) { p->w_open++; p->had_writer = 1; } else p->r_open++;
}

void pipe_close_end(int idx, int write_end) {
    struct kpipe *p = pp(idx); if (!p) return;
    if (write_end) { if (p->w_open > 0) p->w_open--; if (p->w_open == 0 && p->rw) { task_wake(p->rw); p->rw = 0; } }  /* readers see EOF */
    else           { if (p->r_open > 0) p->r_open--; if (p->r_open == 0 && p->ww) { task_wake(p->ww); p->ww = 0; } }  /* writers get EPIPE */
    if (p->r_open == 0 && p->w_open == 0 && !p->pinned) p->used = 0;   /* both ends gone -> free (a FIFO's pinned pipe persists) */
}
