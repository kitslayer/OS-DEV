/*
 * fanfs.c — the /fan userspace-materialized filesystem. See fanfs.h.
 *
 * The reader (fanfs_read) and the daemon (fanfs_wait/fanfs_provide) meet at a
 * single in-flight request, the same two-way rendezvous as the seccomp-notify
 * supervisor (M1124): the reader parks with its request, the daemon wakes and
 * answers, the reader resumes with the bytes. Content is staged in a kernel
 * buffer so it crosses the two address spaces safely (each side copies while its
 * own CR3 is active).
 */
#include "fanfs.h"
#include "task.h"

/* M1621: same fix as mbox.c(M1608)/pty.c(M1607)/app.c's seccomp-notify(M1612)
 * -- this file's own header comment above says it's "the same two-way
 * rendezvous as the seccomp-notify supervisor," but never got the lock that
 * rendezvous needed once M1531 made the reader and the daemon genuinely
 * concurrent on different cores. One lock for the whole file. */
static volatile int fan_lock;
static inline uint64_t fan_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    while (__atomic_exchange_n(&fan_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return f;
}
static inline void fan_irq_restore(uint64_t f) {
    __atomic_store_n(&fan_lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory", "cc");
}

#define FAN_NAME 64
#define FAN_DATA 256

static task_t *g_daemon;          /* the registered /fan daemon (0 = none) */
static int     g_daemon_waiting;  /* daemon parked in fanfs_wait */
static struct {
    int      active;              /* a request is in flight */
    int      provided;           /* the daemon has staged content */
    char     name[FAN_NAME];
    char     data[FAN_DATA];
    int      data_len;
    task_t  *reader;             /* the parked reader */
} g_req;

static void scopy(char *d, const char *s, int max) { int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int  slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

long fanfs_serve(void) {
    g_daemon = task_self();
    return 0;
}

long fanfs_read(const char *name, void *buf, unsigned long max) {
    uint64_t f = fan_irq_save();
    if (!g_daemon) { fan_irq_restore(f); return -1; }         /* no daemon registered */
    if (g_req.active) { fan_irq_restore(f); return -1; }      /* one request at a time (busy) */
    scopy(g_req.name, name, FAN_NAME);
    g_req.active = 1; g_req.provided = 0; g_req.data_len = 0;
    g_req.reader = task_self();
    if (g_daemon_waiting) { task_wake(g_daemon); g_daemon_waiting = 0; }  /* wake the daemon */
    fan_irq_restore(f);
    for (;;) {
        f = fan_irq_save();
        if (g_req.provided) break;             /* daemon answered: fall through with the lock held */
        if (!g_daemon) { g_req.active = 0; fan_irq_restore(f); return -1; }   /* daemon vanished */
        fan_irq_restore(f);                    /* released BEFORE blocking (M1621) */
        task_block();
    }
    int n = g_req.data_len; if (n > (int)max) n = (int)max;
    for (int i = 0; i < n; i++) ((char *)buf)[i] = g_req.data[i];
    g_req.active = 0;                          /* request complete */
    fan_irq_restore(f);
    return n;
}

long fanfs_wait(char *namebuf, int max) {
    if (task_self() != g_daemon) return -1;    /* only the daemon waits */
    uint64_t f;
    for (;;) {
        f = fan_irq_save();
        if (g_req.active && !g_req.provided) break;   /* fresh, unanswered request: fall through with the lock held */
        g_daemon_waiting = 1;
        fan_irq_restore(f);                    /* released BEFORE blocking (M1621) */
        task_block();
        uint64_t f2 = fan_irq_save(); g_daemon_waiting = 0; fan_irq_restore(f2);
    }
    int n = slen(g_req.name); if (n > max) n = max;
    for (int i = 0; i < n; i++) namebuf[i] = g_req.name[i];
    fan_irq_restore(f);
    return n;
}

long fanfs_provide(const void *content, unsigned long len) {
    uint64_t f = fan_irq_save();
    if (task_self() != g_daemon || !g_req.active || g_req.provided) { fan_irq_restore(f); return -1; }
    int n = (int)len; if (n > FAN_DATA) n = FAN_DATA;
    for (int i = 0; i < n; i++) g_req.data[i] = ((const char *)content)[i];
    g_req.data_len = n; g_req.provided = 1;
    if (g_req.reader) task_wake(g_req.reader);  /* resume the reader */
    fan_irq_restore(f);
    return n;
}
