/*
 * shm.h — named shared memory (POSIX-style /dev/shm in spirit).
 *
 * A small table of named objects, each a set of physical frames allocated on
 * first open. app_shm_open maps an object's frames into the calling app's
 * address space (eagerly, refcounted via the M1089 per-frame refcount), so two
 * mappings — in the same process or two different ones — share the same RAM:
 * a write through one is seen through the other. The frames persist for the
 * session (owned by the table at refcount 0; each mapping adds/drops a ref).
 */
#pragma once
#include <stdint.h>

/* Get (creating on first use) the frame list for `name` sized to hold `size`
 * bytes. *frames -> the physical-frame array, *npages -> its length. 0/-1. */
int shm_get(const char *name, uint64_t size, uint64_t **frames, int *npages);
int shm_format(char *out, int max);   /* /proc/shm: objects + sizes */
int shm_unlink(const char *name);     /* remove the name -> object association; 0/-1 (M1590) */
uint64_t shm_max_bytes(void);         /* the real per-object size cap shm_get enforces (SHM_MAXPAGES*PAGE_SIZE) (M1592) */
