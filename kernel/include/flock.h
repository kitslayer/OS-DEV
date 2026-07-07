#pragma once
/*
 * flock.h — advisory whole-file locks, keyed by PATH (M1177).
 *
 * Mutual exclusion between processes on a named file. Because this OS's VFS is
 * name-based with no fd table, locks are keyed by the pathname (not an fd) — a
 * cleaner fit here than the Unix fd-keyed flock(2). A shared (LOCK_SH) lock
 * coexists with other shared locks; an exclusive (LOCK_EX) lock conflicts with
 * any other holder. Without LOCK_NB a conflicting request blocks until the lock
 * is free. A process's locks are released when it unlocks or exits.
 */

int  flock_op(const char *path, int pid, int op);   /* LOCK_SH/EX/UN | LOCK_NB; 0 ok / -1 (M1177) */
void flock_release_pid(int pid);                     /* release every lock a pid holds (process exit) */
int  flock_format(char *b, int max);                 /* /proc/locks table */
/* POSIX fcntl byte-range record locks (M1221) — a separate lock space.
 * can_block: 0 for F_SETLK (fail immediately on conflict), 1 for F_SETLKW
 * (block until the conflicting range unlocks, like flock_op's own LOCK_NB-
 * less path) (M1597). Always non-blocking for F_UNLCK, matching real fcntl. */
int  rlock_set(const char *path, int pid, int type, long start, long len, int can_block);   /* F_SETLK/F_SETLKW/F_UNLCK; 0/-1 */
int  rlock_get(const char *path, int pid, int type, long start, long len,
               int *out_pid, int *out_type, long *out_start, long *out_len);  /* F_GETLK; 1 if a conflict, else 0 */
