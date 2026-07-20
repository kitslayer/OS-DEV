/*
 * journal.h — a from-scratch write-ahead journal for crash consistency (M1864).
 *
 * The problem: an OS-DEV filesystem operation (e.g. FAT32 "create file" = update
 * the FAT + write a directory entry + write the data cluster) is several separate
 * sector writes. A power loss / reset between them leaves the filesystem torn
 * (a directory entry pointing at an unallocated cluster, a half-updated FAT, ...).
 *
 * The fix, the classic ext3/jbd approach: a WRITE-AHEAD LOG. All the sector
 * writes of one logical operation are first written to a reserved on-disk
 * JOURNAL region together with a COMMIT record; only after the commit record is
 * durable are the writes copied ("checkpointed") to their real locations. On the
 * next mount, journal_recover() REPLAYS any transaction whose commit record is
 * present (re-applying its writes — idempotent) and DISCARDS any transaction
 * whose commit record is missing (a crash before commit — the real locations
 * were never touched). So every transaction is atomic across a crash:
 * all-or-nothing, never torn.
 *
 * The journal core is written against an ABSTRACT block device (function
 * pointers) so the exact same logic runs (a) in the kernel over the ata /
 * blockdev drivers and (b) in a host unit test over an in-memory array with
 * fault injection at every crash point — see tests/journal_test.c (the
 * crash-consistency proof).
 */
#ifndef JOURNAL_H
#define JOURNAL_H
#include <stdint.h>

#define JRNL_BLK        512u              /* the journal works in 512-byte blocks */
#define JRNL_DMAGIC     0x314C424Au       /* "JBL1" — descriptor block magic */
#define JRNL_CMAGIC     0x314D434Au       /* "JCM1" — commit block magic     */
#define JRNL_MAXTXN     62                /* max data blocks per transaction (descriptor targets[] fits 62 in one 512B block) */
#define JRNL_MINLEN     (JRNL_MAXTXN + 2) /* journal region must be >= this many blocks (descriptor + data + commit) */

/* Abstract 512-byte-block device the journal sits on. read/write return 0 on
 * success, <0 on error; flush makes prior writes durable (NULL if the device is
 * already write-through). All LBAs are absolute device block numbers. */
typedef struct {
    int  (*read)(void *ctx, uint64_t lba, void *buf);
    int  (*write)(void *ctx, uint64_t lba, const void *buf);
    void (*flush)(void *ctx);
    void *ctx;
    uint64_t jstart;      /* first LBA of the reserved journal region */
    uint32_t jlen;        /* journal region length in blocks (>= JRNL_MINLEN) */
    /* --- runtime state (not on disk) --- */
    uint32_t seq;         /* next transaction sequence number */
    int      in_txn;      /* a transaction is open */
    int      npend;       /* staged blocks in the open transaction */
    uint64_t pend_lba[JRNL_MAXTXN];
    uint8_t  pend_buf[JRNL_MAXTXN][JRNL_BLK];
    int      dbg_crash;   /* TEST ONLY: if set, commit stops right AFTER the commit
                             point (before checkpoint), returning 2 — simulates a
                             power loss with a committed-but-uncheckpointed txn that
                             journal_recover() must then replay. Zero in normal use. */
} journal_t;

/* Initialise an EMPTY journal (writes a clean/invalid descriptor). Call once
 * when creating a fresh journal region. Returns 0, or <0 on I/O error. */
int journal_format(journal_t *j);

/* Crash recovery — call on mount, BEFORE any new writes. Replays a committed-
 * but-not-yet-checkpointed transaction (if any) and clears the journal.
 * Returns the number of transactions replayed (0 or 1), or <0 on error. */
int journal_recover(journal_t *j);

/* Begin / stage / commit a transaction. begin opens it; write stages one block
 * (up to JRNL_MAXTXN); commit durably logs + checkpoints them atomically. After
 * commit returns 0 the real LBAs hold the new data. abort discards the staged
 * blocks without touching any real LBA. write returns <0 if the txn is full. */
int  journal_begin(journal_t *j);
int  journal_write(journal_t *j, uint64_t lba, const void *buf);
int  journal_commit(journal_t *j);
void journal_abort(journal_t *j);

/* Read-back of a block STAGED in the current (open) transaction. A filesystem
 * doing read-modify-write inside a transaction (e.g. two FAT entries in one
 * sector) must see its own earlier staged write, not the stale on-disk version.
 * Returns 1 and fills buf if lba is staged; 0 otherwise. */
int  journal_peek(journal_t *j, uint64_t lba, void *buf);

/* Blocks the open transaction can still take before it is full (0 => no txn). */
int  journal_room(journal_t *j);

#endif
