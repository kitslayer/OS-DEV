/*
 * bcache.c — the unified LRU block cache (M1869). See bcache.h.
 *
 * One shared pool replaces ata.c's ACACHE and blockdev.c's per-layer buffer
 * cache. Keyed by (owner, lba); recency-ordered by a global tick. The internal
 * spinlock-in-cli guards only the fast scan/install bookkeeping — NEVER disk I/O
 * (callers read/write the disk outside these calls and then install), and every
 * lookup copies the block into the caller's buffer, so an eviction can never
 * leave a dangling pointer. Write-through, so a crash can't lose a cached write.
 */
#include "bcache.h"
#include "string.h"

#define BCACHE_N 128                 /* 128 * 512 B = 64 KiB (was 32 KiB ACACHE + 32 KiB bcache) */

static struct bcent {
    int      valid;
    uint32_t owner;
    uint64_t lba, lru;
    uint8_t  data[BCACHE_SECSZ];
} g_bc[BCACHE_N];
static uint64_t g_clk, g_hits, g_miss;

/* cli + a spinlock: cli alone only stops a local reentry, not another core
 * (ring-3 tasks run on every core since M1531), so guard the shared pool for
 * real. Held only across the in-memory scan/install, never across disk I/O. */
static volatile int g_lock;
static inline uint64_t bc_lock(void) {
    uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return f;
}
static inline void bc_unlock(uint64_t f) {
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory", "cc");
}

int bcache_lookup(uint32_t owner, uint64_t lba, void *buf) {
    uint64_t f = bc_lock();
    for (int k = 0; k < BCACHE_N; k++)
        if (g_bc[k].valid && g_bc[k].owner == owner && g_bc[k].lba == lba) {
            memcpy(buf, g_bc[k].data, BCACHE_SECSZ);
            g_bc[k].lru = ++g_clk; g_hits++;
            bc_unlock(f); return 1;
        }
    g_miss++;
    bc_unlock(f);
    return 0;
}

void bcache_install(uint32_t owner, uint64_t lba, const void *buf) {
    uint64_t f = bc_lock();
    int v = -1; uint64_t oldest = ~0ull;
    for (int k = 0; k < BCACHE_N; k++)                   /* already cached? refresh that slot */
        if (g_bc[k].valid && g_bc[k].owner == owner && g_bc[k].lba == lba) { v = k; break; }
    if (v < 0)                                           /* else the first empty slot, else the LRU victim */
        for (int k = 0; k < BCACHE_N; k++) {
            if (!g_bc[k].valid) { v = k; break; }
            if (g_bc[k].lru < oldest) { oldest = g_bc[k].lru; v = k; }
        }
    if (v < 0) v = 0;
    g_bc[v].valid = 1; g_bc[v].owner = owner; g_bc[v].lba = lba; g_bc[v].lru = ++g_clk;
    memcpy(g_bc[v].data, buf, BCACHE_SECSZ);
    bc_unlock(f);
}

void bcache_inval(uint32_t owner, uint64_t lba) {
    uint64_t f = bc_lock();
    for (int k = 0; k < BCACHE_N; k++)
        if (g_bc[k].valid && g_bc[k].owner == owner && g_bc[k].lba == lba) g_bc[k].valid = 0;
    bc_unlock(f);
}

void bcache_inval_range(uint32_t owner, uint64_t lba, uint32_t count) {
    uint64_t f = bc_lock();
    for (int k = 0; k < BCACHE_N; k++)
        if (g_bc[k].valid && g_bc[k].owner == owner && g_bc[k].lba >= lba && g_bc[k].lba < lba + count)
            g_bc[k].valid = 0;
    bc_unlock(f);
}

void bcache_inval_owner(uint32_t owner) {
    uint64_t f = bc_lock();
    for (int k = 0; k < BCACHE_N; k++)
        if (g_bc[k].valid && g_bc[k].owner == owner) g_bc[k].valid = 0;
    bc_unlock(f);
}

void bcache_flush(void) {
    uint64_t f = bc_lock();
    for (int k = 0; k < BCACHE_N; k++) g_bc[k].valid = 0;
    bc_unlock(f);
}

void bcache_counts(uint64_t *hits, uint64_t *miss) {
    uint64_t f = bc_lock();
    if (hits) *hits = g_hits; if (miss) *miss = g_miss;
    bc_unlock(f);
}

int bcache_stats(char *out, int max) {
    uint64_t f = bc_lock();
    int used = 0; for (int k = 0; k < BCACHE_N; k++) if (g_bc[k].valid) used++;
    uint64_t hits = g_hits, miss = g_miss;
    bc_unlock(f);
    /* tiny manual formatter (no snprintf in the freestanding kernel) */
    int n = 0;
    const char *labels[4] = { "entries ", " used ", " hits ", " miss " };
    uint64_t vals[4] = { (uint64_t)BCACHE_N, (uint64_t)used, hits, miss };
    for (int i = 0; i < 4 && n < max - 24; i++) {
        for (const char *s = labels[i]; *s && n < max - 24; s++) out[n++] = *s;
        char tmp[24]; int t = 0; uint64_t v = vals[i];
        if (!v) tmp[t++] = '0'; else while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) out[n++] = tmp[--t];
    }
    if (n < max) out[n++] = '\n';
    if (n < max) out[n] = 0;
    return n;
}
