/*
 * bcache.h — one unified block cache for the whole kernel (M1869).
 *
 * Before this, two separate 512-byte block caches existed: ata.c's ACACHE (the
 * boot disk's read cache, keyed by ATA drive) and blockdev.c's LRU buffer cache
 * (keyed by blockdev device index). They duplicated the same logic and
 * double-cached any ATA disk reached through the blockdev layer. This is the
 * single shared pool both now use — one LRU, one set of hit/miss stats
 * (/proc/bcache), one implementation — with each caller passing an `owner` id so
 * the ATA and blockdev key namespaces don't collide.
 *
 * Write-through (a write updates the cached copy AND the disk), so the cache
 * never holds data the disk doesn't — a crash can't lose a cached write. It is a
 * pure LRU read cache + coherence point; correctness never depends on it (every
 * lookup copies into the CALLER's buffer, so an eviction can't dangle).
 */
#ifndef BCACHE_H
#define BCACHE_H
#include <stdint.h>

#define BCACHE_SECSZ 512

/* Owner namespaces so ATA drives and blockdev devices don't alias each other. */
#define BCACHE_OWNER_ATA(drive) ((uint32_t)(drive))          /* 0..15  : ATA drives */
#define BCACHE_OWNER_BLK(dev)   ((uint32_t)(16 + (dev)))     /* 16..   : blockdev devices */

/* Read one 512-byte block from the cache: 1 = hit (copied into buf), 0 = miss. */
int  bcache_lookup(uint32_t owner, uint64_t lba, void *buf);
/* Install / refresh a block in the cache (after a disk read, or a write-through). */
void bcache_install(uint32_t owner, uint64_t lba, const void *buf);
/* Drop one block / a [lba, lba+count) range / all of one owner's / everything. */
void bcache_inval(uint32_t owner, uint64_t lba);
void bcache_inval_range(uint32_t owner, uint64_t lba, uint32_t count);
void bcache_inval_owner(uint32_t owner);
void bcache_flush(void);
/* /proc/bcache: entries, hits, misses. Returns bytes written. */
int  bcache_stats(char *out, int max);
/* Numeric hit/miss counters (for self-tests). */
void bcache_counts(uint64_t *hits, uint64_t *miss);

#endif
