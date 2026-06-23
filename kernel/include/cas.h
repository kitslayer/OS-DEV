/*
 * cas.h — a content-addressed blob store (git / Plan 9 Venti / IPFS style).
 *
 * Every blob is keyed by the SHA-256 of its own bytes, so identical content is
 * stored exactly once (automatic dedup) and a fetch can re-hash on the way out
 * to guarantee integrity — corruption or tampering is detected, not returned.
 * The store is append-only (no free list, no fragmentation): store appends,
 * fetch reads. Built on the kernel's SHA-256. A from-scratch take on the core
 * primitive behind git objects, restic/borg, and IPFS.
 */
#pragma once
#include <stdint.h>

/* Store `len` bytes; write the 32-byte content key (SHA-256) to out_hash.
 * Returns 0 on success (whether freshly stored or already present — dedup),
 * -1 if the store is full. */
int  cas_store(const void *data, uint32_t len, uint8_t out_hash[32]);

/* Fetch the blob with key `hash` into `out` (capacity `max`). Re-hashes the
 * stored bytes and rejects a mismatch. Returns the blob length, or -1 if not
 * found / integrity check failed / out too small. */
long cas_fetch(const uint8_t hash[32], void *out, uint32_t max);

/* Format store statistics (objects, bytes used, dedup hits, capacity) into
 * `out` (capacity `max`); returns the byte length. Backs /proc/cas. */
int  cas_format(char *out, int max);
