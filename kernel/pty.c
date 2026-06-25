/*
 * pty.c — pseudoterminal master/slave pairs + a kernel line discipline (M1185).
 * See pty.h. Mirrors unixsock.c: a fixed table, byte rings, block-once-when-empty
 * woken by the peer (lost-wakeup-free on this single CPU because the int-0x80 gate
 * runs the syscall with IF=0, so "empty? record waiter; block" is atomic against a
 * writer that can only run after we've blocked). No fd table — the small integer
 * id (idx<<1 | side) is the handle and stays valid across fork().
 *
 * Two rings per pty: `in` (slave-readable input, produced by the master through
 * the line discipline) and `out` (master-readable output: slave program output +
 * canonical echoes). Canonical mode line-buffers into `line` and only commits a
 * completed line to `in`; raw mode passes bytes straight through. INTR (^C) flushes
 * the line and delivers SIGINT to the slave's foreground process group.
 */
#include "pty.h"
#include "task.h"
#include "app.h"        /* app_killpg, app_sys_getpid */
#include "syscall.h"    /* ICANON, ECHO, ISIG, VINTR/VEOF/VERASE/VKILL */

#define SIGINT 2        /* matches app.c's local define (not in a shared header) */
#define NPTY   8
#define PBUF   1024

struct ptyring { unsigned char b[PBUF]; int head, tail; };   /* empty when head==tail */
static int pr_cnt(struct ptyring *r)  { return (r->head - r->tail + PBUF) % PBUF; }
static int pr_free(struct ptyring *r) { return PBUF - 1 - pr_cnt(r); }   /* one slot kept empty */
static int pr_put(struct ptyring *r, const unsigned char *d, int n) {
    int w = 0; while (w < n && pr_free(r) > 0) { r->b[r->head] = d[w++]; r->head = (r->head + 1) % PBUF; } return w;
}
static int pr_get(struct ptyring *r, unsigned char *d, int n) {
    int g = 0; while (g < n && pr_cnt(r) > 0) { d[g++] = r->b[r->tail]; r->tail = (r->tail + 1) % PBUF; } return g;
}

struct pty {
    int used, owner;                 /* owner pid (for cleanup) */
    int m_open, s_open;              /* which ends are still open */
    unsigned lflag;                  /* ICANON | ECHO | ISIG */
    unsigned char cc[4];             /* VINTR, VEOF, VERASE, VKILL */
    int fg_pgid;                     /* INTR signal target (pty_ctl cmd 1) */
    unsigned short ws_rows, ws_cols; /* terminal window size (TIOCGWINSZ/SWINSZ, M1279) */
    int eof;                         /* VEOF on an empty line -> slave read sees EOF */
    struct ptyring in, out;          /* in: slave-readable; out: master-readable */
    unsigned char line[PBUF]; int linelen;   /* canonical editing buffer (uncommitted) */
    task_t *in_waiter, *out_waiter;  /* a blocked slave reader / master reader */
};
static struct pty ptys[NPTY];

/* Push a byte to the master-readable output ring + wake a blocked master reader. */
static void emit(struct pty *p, unsigned char c) {
    pr_put(&p->out, &c, 1);
    if (p->out_waiter) { task_wake(p->out_waiter); p->out_waiter = 0; }
}
/* Commit the canonical line buffer to slave-readable input + wake a slave reader. */
static void commit(struct pty *p) {
    if (p->linelen > 0) pr_put(&p->in, p->line, p->linelen);
    p->linelen = 0;
    if (p->in_waiter) { task_wake(p->in_waiter); p->in_waiter = 0; }
}

/* Process one master-written byte through the line discipline. */
static void ldisc(struct pty *p, unsigned char c) {
    if ((p->lflag & ISIG) && c == p->cc[VINTR]) {            /* ^C: flush + signal */
        p->linelen = 0;
        if (p->lflag & ECHO) { emit(p, '^'); emit(p, 'C'); emit(p, '\n'); }
        if (p->fg_pgid > 0) app_killpg(p->fg_pgid, SIGINT);
        return;
    }
    if (!(p->lflag & ICANON)) {                              /* raw: deliver immediately */
        pr_put(&p->in, &c, 1);
        if (p->lflag & ECHO) emit(p, c);
        if (p->in_waiter) { task_wake(p->in_waiter); p->in_waiter = 0; }
        return;
    }
    if (c == p->cc[VERASE] || c == 127) {                    /* backspace: rub out a char */
        if (p->linelen > 0) { p->linelen--; if (p->lflag & ECHO) { emit(p, '\b'); emit(p, ' '); emit(p, '\b'); } }
        return;
    }
    if (c == p->cc[VKILL]) {                                 /* ^U: erase the whole line */
        while (p->linelen > 0) { p->linelen--; if (p->lflag & ECHO) { emit(p, '\b'); emit(p, ' '); emit(p, '\b'); } }
        return;
    }
    if (c == p->cc[VEOF]) {                                  /* ^D: commit; empty line => EOF */
        if (p->linelen == 0) { p->eof = 1; if (p->in_waiter) { task_wake(p->in_waiter); p->in_waiter = 0; } }
        else commit(p);
        return;
    }
    if (p->lflag & ECHO) emit(p, c);
    if (p->linelen < (int)sizeof p->line) p->line[p->linelen++] = c;
    if (c == '\n') commit(p);                                /* newline ends the line */
}

int pty_open(void) {
    for (int i = 0; i < NPTY; i++) {
        if (ptys[i].used) continue;
        struct pty *p = &ptys[i];
        for (unsigned k = 0; k < sizeof *p; k++) ((unsigned char *)p)[k] = 0;
        p->used = 1; p->m_open = 1; p->s_open = 1;
        p->owner = app_sys_getpid();
        p->fg_pgid = p->owner;                               /* INTR targets the opener by default */
        p->lflag = ICANON | ECHO | ISIG;                     /* cooked, like a real new tty */
        p->cc[VINTR] = 3; p->cc[VEOF] = 4; p->cc[VERASE] = 8; p->cc[VKILL] = 21;
        return i << 1;                                       /* master id (even); slave = this|1 */
    }
    return -1;
}

/* Is pair `n` a live pty whose slave (/dev/pts/n) can be opened? (master open) */
int pty_pts_valid(int n) { return n >= 0 && n < NPTY && ptys[n].used && ptys[n].m_open; }

static struct pty *resolve(int id, int *is_slave) {
    if (id < 0) return 0;
    int idx = id >> 1;
    if (idx >= NPTY || !ptys[idx].used) return 0;
    *is_slave = id & 1;
    return &ptys[idx];
}

long pty_write(int id, const void *buf, unsigned long len) {
    int slave; struct pty *p = resolve(id, &slave); if (!p) return -1;
    const unsigned char *d = (const unsigned char *)buf;
    if (!slave) {                                            /* MASTER write -> line discipline */
        for (unsigned long i = 0; i < len; i++) ldisc(p, d[i]);
        return (long)len;
    }
    int n = pr_put(&p->out, d, (int)len);                    /* SLAVE write -> program output */
    if (n > 0 && p->out_waiter) { task_wake(p->out_waiter); p->out_waiter = 0; }
    return n;
}

long pty_read(int id, void *buf, unsigned long max) {
    int slave; struct pty *p = resolve(id, &slave); if (!p) return -1;
    struct ptyring *r = slave ? &p->in : &p->out;
    for (;;) {
        if (pr_cnt(r) > 0) break;
        if (slave) { if (p->eof || !p->m_open) return 0; p->in_waiter = task_self(); }
        else       { if (!p->s_open) return 0;            p->out_waiter = task_self(); }
        task_block();                                        /* woken by a writer/close (or a kill) */
        if (!p->used) return -1;
    }
    return pr_get(r, (unsigned char *)buf, (int)max);
}

int pty_close(int id) {
    int slave; struct pty *p = resolve(id, &slave); if (!p) return -1;
    if (slave) p->s_open = 0; else p->m_open = 0;
    if (p->in_waiter)  { task_wake(p->in_waiter);  p->in_waiter = 0; }   /* let blocked reads see EOF */
    if (p->out_waiter) { task_wake(p->out_waiter); p->out_waiter = 0; }
    if (!p->m_open && !p->s_open) p->used = 0;               /* both ends gone -> free the slot */
    return 0;
}

int pty_ctl(int id, int cmd, int arg) {
    int slave; struct pty *p = resolve(id, &slave); if (!p) return -1;
    if (cmd == 0) { p->lflag = (unsigned)arg & (ICANON | ECHO | ISIG); return 0; }   /* set line-discipline mode */
    if (cmd == 1) { p->fg_pgid = arg; return 0; }                                    /* set INTR target pgid */
    if (cmd == 2) {                                                                  /* TIOCSWINSZ: arg = rows<<16 | cols (M1279) */
        p->ws_rows = (unsigned short)((arg >> 16) & 0xFFFF);
        p->ws_cols = (unsigned short)(arg & 0xFFFF);
        if (p->fg_pgid > 0) app_killpg(p->fg_pgid, SIGWINCH);                        /* notify the foreground group of the resize */
        return 0;
    }
    if (cmd == 3) return (int)(((unsigned)p->ws_rows << 16) | p->ws_cols);           /* TIOCGWINSZ: rows<<16 | cols */
    return -1;
}

int pty_ready(int id) {                                      /* fswait peek */
    int slave; struct pty *p = resolve(id, &slave); if (!p) return 0;
    if (slave) return pr_cnt(&p->in) > 0 || p->eof || !p->m_open;
    return pr_cnt(&p->out) > 0 || !p->s_open;
}

void pty_release_pid(int pid) {
    for (int i = 0; i < NPTY; i++)
        if (ptys[i].used && ptys[i].owner == pid) {
            if (ptys[i].in_waiter)  { task_wake(ptys[i].in_waiter);  ptys[i].in_waiter = 0; }
            if (ptys[i].out_waiter) { task_wake(ptys[i].out_waiter); ptys[i].out_waiter = 0; }
            ptys[i].used = 0;
        }
}
