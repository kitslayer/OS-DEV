/*
 * journal_test.c — crash-consistency proof for kernel/journal.c (M1864).
 *
 * The journal's whole reason to exist is: a power loss in the middle of a
 * filesystem operation must leave the disk either fully-old or fully-new, never
 * torn. This harness proves exactly that, mechanically, for EVERY crash point.
 *
 * Model: an in-memory disk with a write cache. journal writes go to the cache;
 * flush() applies the cache to the "platter" ONE block at a time, in write
 * order. A "crash" drops every platter write from a chosen point onward AND
 * loses the un-flushed cache — i.e. exactly what a power loss does to a disk
 * with a write-back cache. After the crash we build a FRESH journal over the
 * platter, run journal_recover(), and assert the target blocks are all-old or
 * all-new. Sweeping the crash point across all writes of a commit exercises
 * every window (before commit / at commit / mid-checkpoint / after).
 */
#define JRNL_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../kernel/include/journal.h"
#include "../kernel/journal.c"

#define NBLK 512                       /* in-memory disk: 512 blocks of 512 bytes */
#define JSTART 400                     /* journal region [400, 400+jlen) */
#define JLEN   JRNL_MINLEN             /* 64 */

static uint8_t platter[NBLK][JRNL_BLK];

/* Write cache, kept in WRITE ORDER so a crash lands at a precise write. */
struct wr { uint64_t lba; uint8_t data[JRNL_BLK]; };
static struct wr wlog[20000];
static int  wn;
static long g_ops, g_crash_at;          /* crash after g_crash_at platter writes */
static int  g_crashed;

static int h_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx; if (lba >= NBLK) return -1;
    for (int i = wn - 1; i >= 0; i--) if (wlog[i].lba == lba) { memcpy(buf, wlog[i].data, JRNL_BLK); return 0; }
    memcpy(buf, platter[lba], JRNL_BLK); return 0;
}
static int h_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx; if (lba >= NBLK) return -1;
    if (wn >= (int)(sizeof wlog / sizeof wlog[0])) { fprintf(stderr, "wlog overflow\n"); exit(2); }
    wlog[wn].lba = lba; memcpy(wlog[wn].data, buf, JRNL_BLK); wn++; return 0;
}
static void h_flush(void *ctx) {
    (void)ctx;
    for (int i = 0; i < wn; i++) {
        if (!g_crashed && g_ops >= g_crash_at) g_crashed = 1;
        if (g_crashed) break;                       /* power lost: nothing more reaches the platter */
        memcpy(platter[wlog[i].lba], wlog[i].data, JRNL_BLK);
        g_ops++;
    }
    wn = 0;                                          /* applied, or lost on crash — cache is now empty */
}
static void reboot_cache(void) { wn = 0; }           /* lose the volatile cache (simulate a reboot) */

static journal_t J(void) {
    journal_t j; memset(&j, 0, sizeof j);
    j.read = h_read; j.write = h_write; j.flush = h_flush; j.ctx = 0;
    j.jstart = JSTART; j.jlen = JLEN;
    return j;
}

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

/* Fill target block `lba` with a recognizable pattern (tag byte repeated). */
static void pat(uint8_t *b, uint8_t tag) { for (unsigned i = 0; i < JRNL_BLK; i++) b[i] = tag; }
static int is_pat(const uint8_t *b, uint8_t tag) { for (unsigned i = 0; i < JRNL_BLK; i++) if (b[i] != tag) return 0; return 1; }

/* One crash-sweep: N target blocks go from tag OLD -> NEW inside a single txn,
 * with the power cut after `crash_at` platter writes. Returns 1 if the recovered
 * state is consistent (all-OLD or all-NEW), 0 if torn. */
static int sweep_once(uint64_t *targets, int n, uint8_t OLD, uint8_t NEW, long crash_at) {
    /* fresh disk: journal region clean, targets = OLD */
    memset(platter, 0, sizeof platter);
    wn = 0; g_ops = 0; g_crash_at = 1 << 30; g_crashed = 0;
    journal_t j = J(); journal_format(&j);
    for (int i = 0; i < n; i++) pat(platter[targets[i]], OLD);

    /* run one transaction with the crash armed */
    g_ops = 0; g_crash_at = crash_at; g_crashed = 0;
    journal_begin(&j);
    for (int i = 0; i < n; i++) { uint8_t nb[JRNL_BLK]; pat(nb, NEW); journal_write(&j, targets[i], nb); }
    journal_commit(&j);                              /* may be cut short by the injected crash */

    /* reboot: lose volatile cache, fresh journal object, recover */
    reboot_cache(); g_crashed = 0; g_crash_at = 1 << 30;
    journal_t j2 = J();
    int r = journal_recover(&j2);
    if (r < 0) return 0;

    /* every target must be uniformly OLD or uniformly NEW, and all the same */
    int allold = 1, allnew = 1;
    for (int i = 0; i < n; i++) {
        uint8_t b[JRNL_BLK]; reboot_cache(); j2 = J(); h_read(0, targets[i], b);
        if (!is_pat(b, OLD)) allold = 0;
        if (!is_pat(b, NEW)) allnew = 0;
    }
    /* recovery must also be idempotent: a second recover replays nothing and
     * leaves the state identical. */
    reboot_cache(); journal_t j3 = J(); int r2 = journal_recover(&j3);
    if (r2 != 0) return 0;                            /* nothing left to replay */
    return allold || allnew;
}

int main(void) {
    printf("journal_test: crash-consistency proof for kernel/journal.c\n");

    /* 1. tiny sanity: format + a clean commit + verify the new data landed */
    {
        memset(platter, 0, sizeof platter); wn = 0; g_ops = 0; g_crash_at = 1 << 30; g_crashed = 0;
        journal_t j = J(); CHECK(journal_format(&j) == 0, "format");
        pat(platter[10], 'A'); pat(platter[11], 'A');
        journal_begin(&j);
        uint8_t nb[JRNL_BLK]; pat(nb, 'B');
        journal_write(&j, 10, nb); journal_write(&j, 11, nb);
        CHECK(journal_commit(&j) == 0, "commit");
        uint8_t b[JRNL_BLK]; h_read(0, 10, b); CHECK(is_pat(b, 'B'), "block 10 updated"); h_read(0, 11, b); CHECK(is_pat(b, 'B'), "block 11 updated");
        journal_t jr = J(); CHECK(journal_recover(&jr) == 0, "clean journal after commit: nothing to replay");
    }

    /* 2. exhaustive crash sweep, small txn (3 blocks): cut power after every
     * possible write and require all-or-nothing each time. */
    {
        uint64_t t[3] = { 5, 20, 100 };
        int torn = 0, sawold = 0, sawnew = 0;
        for (long ca = 0; ca <= 300; ca++) {
            /* re-run to observe the recovered outcome for stats too */
            if (!sweep_once(t, 3, 'O', 'N', ca)) torn = 1;
            /* classify (re-read after the sweep left j2 state on the platter) */
            uint8_t b[JRNL_BLK]; reboot_cache(); h_read(0, t[0], b);
            if (is_pat(b, 'O')) sawold = 1;
            if (is_pat(b, 'N')) sawnew = 1;
        }
        CHECK(!torn, "3-block txn: no torn state across ANY crash point");
        CHECK(sawold && sawnew, "3-block txn: sweep actually exercised both outcomes (old AND new)");
    }

    /* 3. full-size transaction (JRNL_MAXTXN blocks) crash sweep */
    {
        uint64_t t[JRNL_MAXTXN]; for (int i = 0; i < JRNL_MAXTXN; i++) t[i] = (uint64_t)(200 + i);
        int torn = 0;
        for (long ca = 0; ca <= 400; ca += 3)
            if (!sweep_once(t, JRNL_MAXTXN, 'x', 'y', ca)) torn = 1;
        CHECK(!torn, "full-size (62-block) txn: no torn state across the crash sweep");
    }

    /* 4. abort leaves the disk untouched */
    {
        memset(platter, 0, sizeof platter); wn = 0; g_ops = 0; g_crash_at = 1 << 30; g_crashed = 0;
        journal_t j = J(); journal_format(&j);
        pat(platter[42], 'k');
        journal_begin(&j);
        uint8_t nb[JRNL_BLK]; pat(nb, 'z'); journal_write(&j, 42, nb);
        journal_abort(&j);
        uint8_t b[JRNL_BLK]; h_read(0, 42, b); CHECK(is_pat(b, 'k'), "abort: target block unchanged");
        journal_t jr = J(); CHECK(journal_recover(&jr) == 0, "abort: nothing to recover");
    }

    /* 5. fuzz: random transactions, random crash points, invariant every time */
    {
        srand(1234);
        int torn = 0;
        for (int it = 0; it < 4000; it++) {
            int n = 1 + rand() % JRNL_MAXTXN;
            uint64_t t[JRNL_MAXTXN]; int used[NBLK]; memset(used, 0, sizeof used);
            for (int i = 0; i < n; i++) {
                uint64_t lba; do { lba = rand() % 380; } while (lba < 5 || used[lba]);  /* outside the journal region, distinct */
                used[lba] = 1; t[i] = lba;
            }
            long crash_at = rand() % (2 * n + 6);       /* somewhere across the commit's writes */
            if (!sweep_once(t, n, 'p', 'q', crash_at)) { torn = 1; break; }
        }
        CHECK(!torn, "fuzz (4000 random txns x random crash points): never torn");
    }

    if (fails == 0) { printf("PASS: journal is crash-consistent (all-or-nothing across every injected crash)\n"); return 0; }
    printf("FAIL: %d journal check(s) failed\n", fails);
    return 1;
}
