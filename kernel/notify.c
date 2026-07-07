/*
 * notify.c — named notification objects. See notify.h. A close cousin of
 * mbox.c, but each object is a single coalescing bitmask instead of a message
 * ring: signals OR together until a reader consumes (and clears) them.
 */
#include "notify.h"
#include "task.h"

/* M1621: same fix as mbox.c(M1608)/eventfd.c this same pass -- structurally
 * almost identical to mbox.c pre-M1608 (same waiter field, same shape),
 * never got that fix either. One lock for the whole file (notify_get's
 * scan-then-create race, notify_signal/wait's produce-then-wake vs
 * check-then-block race). */
static volatile int notify_lock;
static inline uint64_t nt_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    while (__atomic_exchange_n(&notify_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return f;
}
static inline void nt_irq_restore(uint64_t f) {
    __atomic_store_n(&notify_lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory", "cc");
}

#define NOTIFY_N 8                  /* up to 8 named notification objects */

struct notify {
    char     name[32];
    int      used;
    uint64_t bits;                  /* OR-accumulated pending signals */
    task_t  *waiter;                /* one blocked reader */
};
static struct notify nt[NOTIFY_N];

static int neq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static struct notify *notify_get(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < NOTIFY_N; i++) if (nt[i].used && neq(nt[i].name, name)) return &nt[i];
    for (int i = 0; i < NOTIFY_N; i++) if (!nt[i].used) {
        int j = 0; while (name[j] && j < 31) { nt[i].name[j] = name[j]; j++; } nt[i].name[j] = 0;
        nt[i].used = 1; nt[i].bits = 0; nt[i].waiter = 0;
        return &nt[i];
    }
    return 0;
}

long notify_signal(const char *name, const void *data, unsigned long len) {
    uint64_t f = nt_irq_save();
    struct notify *n = notify_get(name);
    if (!n) { nt_irq_restore(f); return -1; }
    uint64_t v = 0; const char *s = (const char *)data;
    for (unsigned long i = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (uint64_t)(s[i] - '0');
    if (v == 0) v = 1;                          /* a bare signal (no number) raises bit 0 */
    n->bits |= v;                               /* coalesce */
    if (n->waiter) { task_wake(n->waiter); n->waiter = 0; }
    nt_irq_restore(f);
    return (long)len;
}

long notify_wait(const char *name, void *buf, unsigned long max) {
    uint64_t f = nt_irq_save();
    struct notify *n = notify_get(name);
    if (!n || max < 2) { nt_irq_restore(f); return -1; }
    if (n->bits == 0) {                         /* no pending signals -> block once */
        n->waiter = task_self();
        nt_irq_restore(f);                      /* released BEFORE blocking (M1621) */
        task_block();                           /* woken by a signaller, a keypress, or a kill */
        f = nt_irq_save();
        n->waiter = 0;
        if (n->bits == 0) { nt_irq_restore(f); return 0; }   /* stray wake -> don't re-block */
    }
    uint64_t v = n->bits; n->bits = 0;          /* read-and-clear */
    nt_irq_restore(f);
    char t[24]; int ti = 0; uint64_t x = v;
    if (!x) t[ti++] = '0'; else while (x) { t[ti++] = (char)('0' + x % 10); x /= 10; }
    int p = 0;
    while (ti && (unsigned long)p < max - 1) ((char *)buf)[p++] = t[--ti];
    if ((unsigned long)p < max - 1) ((char *)buf)[p++] = '\n';
    return p;
}

/* Non-blocking readiness peek for fswait (M1125): are any bits pending? Does NOT
 * create the object or clear it. */
int notify_ready(const char *name) {
    for (int i = 0; i < NOTIFY_N; i++) if (nt[i].used && neq(nt[i].name, name)) return nt[i].bits != 0;
    return 0;
}

static int sapp(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int sdec(char *b, int p, int max, uint64_t v) {
    char t[24]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n]; return p;
}

int notify_format(char *b, int max) {
    int p = sapp(b, 0, max, "  OBJECT                PENDING-MASK\n");
    int any = 0;
    for (int i = 0; i < NOTIFY_N; i++) if (nt[i].used) {
        p = sapp(b, p, max, "  "); p = sapp(b, p, max, nt[i].name);
        int nl = 0; while (nt[i].name[nl]) nl++;
        for (int k = nl; k < 20 && p < max - 1; k++) b[p++] = ' ';
        p = sdec(b, p, max, nt[i].bits); p = sapp(b, p, max, "\n");
        any = 1;
    }
    if (!any) p = sapp(b, p, max, "  (none — write to /notify/<name> to create one)\n");
    if (p < max) b[p] = 0;
    return p;
}
