/*
 * journal.c — write-ahead log for crash-consistent filesystem writes (M1864).
 * See journal.h for the design. The core is block-device-agnostic (function
 * pointers) so it runs unchanged in the kernel and in tests/journal_test.c,
 * where a host harness injects a "crash" at every write and proves the on-disk
 * state is always all-or-nothing (never a torn transaction).
 *
 * On-disk layout of the reserved journal region [jstart, jstart+jlen):
 *   jstart+0                : descriptor block  (magic, seq, nblocks, targets[])
 *   jstart+1 .. +JRNL_MAXTXN: staged data blocks (block i at jstart+1+i)
 *   jstart+1+JRNL_MAXTXN     : commit block      (magic, seq, nblocks, csum) — FIXED
 * so jlen must be >= JRNL_MINLEN (JRNL_MAXTXN + 2).
 *
 * Commit order (the whole correctness argument):
 *   1. write descriptor + data blocks to the journal region
 *   2. flush        <- data durable in the journal
 *   3. write commit block
 *   4. flush        <- COMMIT POINT: transaction is now atomic
 *   5. checkpoint   <- copy each staged block to its real target LBA
 *   6. flush; invalidate the descriptor; flush
 * A crash before step 4 leaves the real LBAs untouched (recovery discards the
 * txn). A crash at/after step 4 → recovery replays every staged block to its
 * target (idempotent). Either way the filesystem sees all-or-nothing.
 */
#include "journal.h"
#ifdef JRNL_HOST
#include <string.h>
#else
#include "string.h"
#endif

/* On-disk descriptor block — exactly one 512-byte sector (16 + 62*8 = 512). */
typedef struct {
    uint32_t magic;                 /* JRNL_DMAGIC when a transaction is present */
    uint32_t seq;
    uint32_t nblocks;
    uint32_t reserved;
    uint64_t targets[JRNL_MAXTXN];
} jrnl_desc_t;

/* On-disk commit block (only the first 16 bytes of its sector are used). */
typedef struct {
    uint32_t magic;                 /* JRNL_CMAGIC once the txn is committed */
    uint32_t seq;
    uint32_t nblocks;
    uint32_t csum;                  /* FNV-1a over the nblocks staged data blocks */
} jrnl_commit_t;

/* FNV-1a over the staged data blocks — small, dependency-free; just needs to
 * catch a torn/partial log (device sector writes are themselves atomic). */
static uint32_t jsum_blocks(const uint8_t buf[][JRNL_BLK], int n) {
    uint32_t s = 0x811c9dc5u;
    for (int i = 0; i < n; i++) { const uint8_t *b = buf[i];
        for (unsigned k = 0; k < JRNL_BLK; k++) { s ^= b[k]; s *= 16777619u; } }
    return s;
}

static uint64_t j_desc_lba(journal_t *j)       { return j->jstart; }
static uint64_t j_data_lba(journal_t *j, int i){ return j->jstart + 1 + (uint32_t)i; }
static uint64_t j_commit_lba(journal_t *j)     { return j->jstart + 1 + JRNL_MAXTXN; }

static void j_flush(journal_t *j) { if (j->flush) j->flush(j->ctx); }

/* Is `lba` inside our own journal region? (writing there would eat the log) */
static int j_in_region(journal_t *j, uint64_t lba) {
    return lba >= j->jstart && lba < j->jstart + j->jlen;
}

int journal_format(journal_t *j) {
    if (!j || !j->read || !j->write || j->jlen < JRNL_MINLEN) return -1;
    uint8_t blk[JRNL_BLK];
    memset(blk, 0, sizeof blk);                     /* magic 0 => no transaction present */
    if (j->write(j->ctx, j_desc_lba(j), blk) < 0) return -1;
    j_flush(j);
    j->seq = 1; j->in_txn = 0; j->npend = 0;
    return 0;
}

int journal_recover(journal_t *j) {
    if (!j || !j->read || !j->write || j->jlen < JRNL_MINLEN) return -1;
    j->in_txn = 0; j->npend = 0;
    uint8_t blk[JRNL_BLK];
    jrnl_desc_t d;
    if (j->read(j->ctx, j_desc_lba(j), blk) < 0) return -1;
    memcpy(&d, blk, sizeof d);
    if (d.magic != JRNL_DMAGIC) { j->seq = 1; return 0; }   /* clean */
    j->seq = d.seq + 1;                                     /* continue monotonically */
    if (d.nblocks == 0 || d.nblocks > JRNL_MAXTXN) {        /* corrupt descriptor -> discard */
        memset(blk, 0, sizeof blk); j->write(j->ctx, j_desc_lba(j), blk); j_flush(j); return 0;
    }
    /* Read the commit block + the staged data, then decide. */
    jrnl_commit_t c;
    if (j->read(j->ctx, j_commit_lba(j), blk) < 0) return -1;
    memcpy(&c, blk, sizeof c);
    static uint8_t data[JRNL_MAXTXN][JRNL_BLK];             /* static: keep it off the kernel stack */
    for (uint32_t i = 0; i < d.nblocks; i++)
        if (j->read(j->ctx, j_data_lba(j, (int)i), data[i]) < 0) return -1;
    int committed = (c.magic == JRNL_CMAGIC && c.seq == d.seq && c.nblocks == d.nblocks &&
                     c.csum == jsum_blocks((const uint8_t(*)[JRNL_BLK])data, (int)d.nblocks));
    int replayed = 0;
    if (committed) {                                        /* REPLAY (idempotent) */
        for (uint32_t i = 0; i < d.nblocks; i++)
            if (j->write(j->ctx, d.targets[i], data[i]) < 0) return -1;
        j_flush(j);
        replayed = 1;
    }
    /* Either way, clear the journal so we never replay it twice. */
    memset(blk, 0, sizeof blk);
    if (j->write(j->ctx, j_desc_lba(j), blk) < 0) return -1;
    j_flush(j);
    return replayed;
}

int journal_begin(journal_t *j) {
    if (!j || j->in_txn) return -1;
    j->in_txn = 1; j->npend = 0;
    return 0;
}

int journal_write(journal_t *j, uint64_t lba, const void *buf) {
    if (!j || !j->in_txn) return -1;
    if (j_in_region(j, lba)) return -1;                     /* never let a target eat the journal */
    for (int i = 0; i < j->npend; i++)                      /* last-write-wins on a repeated LBA */
        if (j->pend_lba[i] == lba) { memcpy(j->pend_buf[i], buf, JRNL_BLK); return 0; }
    if (j->npend >= JRNL_MAXTXN) return -1;                 /* transaction full */
    j->pend_lba[j->npend] = lba;
    memcpy(j->pend_buf[j->npend], buf, JRNL_BLK);
    j->npend++;
    return 0;
}

void journal_abort(journal_t *j) { if (j) { j->in_txn = 0; j->npend = 0; } }

int journal_commit(journal_t *j) {
    if (!j || !j->in_txn) return -1;
    int n = j->npend;
    if (n == 0) { j->in_txn = 0; return 0; }                /* empty txn: nothing to do */

    uint8_t blk[JRNL_BLK];
    /* 1. descriptor */
    jrnl_desc_t d; memset(&d, 0, sizeof d);
    d.magic = JRNL_DMAGIC; d.seq = j->seq; d.nblocks = (uint32_t)n;
    for (int i = 0; i < n; i++) d.targets[i] = j->pend_lba[i];
    memset(blk, 0, sizeof blk); memcpy(blk, &d, sizeof d);
    if (j->write(j->ctx, j_desc_lba(j), blk) < 0) return -1;
    /* 1b. staged data blocks */
    for (int i = 0; i < n; i++)
        if (j->write(j->ctx, j_data_lba(j, i), j->pend_buf[i]) < 0) return -1;
    /* 2. flush — descriptor + data durable in the journal before the commit */
    j_flush(j);
    /* 3. commit block */
    jrnl_commit_t c; memset(&c, 0, sizeof c);
    c.magic = JRNL_CMAGIC; c.seq = j->seq; c.nblocks = (uint32_t)n;
    c.csum = jsum_blocks((const uint8_t(*)[JRNL_BLK])j->pend_buf, n);
    memset(blk, 0, sizeof blk); memcpy(blk, &c, sizeof c);
    if (j->write(j->ctx, j_commit_lba(j), blk) < 0) return -1;
    /* 4. flush — COMMIT POINT: the transaction is now atomic across a crash */
    j_flush(j);
    if (j->dbg_crash) { j->in_txn = 0; j->npend = 0; return 2; }  /* TEST: simulate a crash here */
    /* 5. checkpoint — copy each staged block to its real target LBA */
    for (int i = 0; i < n; i++)
        if (j->write(j->ctx, j->pend_lba[i], j->pend_buf[i]) < 0) return -1;
    j_flush(j);
    /* 6. invalidate the descriptor so recovery won't replay a done txn */
    memset(blk, 0, sizeof blk);
    if (j->write(j->ctx, j_desc_lba(j), blk) < 0) return -1;
    j_flush(j);

    j->seq++; j->in_txn = 0; j->npend = 0;
    return 0;
}
