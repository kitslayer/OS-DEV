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
