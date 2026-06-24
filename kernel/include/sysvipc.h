/*
 * sysvipc.h — System V semaphores (M1159). Keyed counting-semaphore sets with
 * atomic all-or-nothing semop + blocking. struct sembuf + the IPC_* / *VAL
 * constants live in syscall.h (shared with userspace). See sysvipc.c.
 */
#pragma once
#include <stdint.h>
#include "syscall.h"   /* struct sembuf, IPC_PRIVATE/CREAT/NOWAIT/RMID, GET/SETVAL */

int sysv_semget(int key, int nsems, int flags);            /* open/create a set; id or -1 */
int sysv_semop(int id, struct sembuf *sops, unsigned nsops); /* atomic all-or-nothing; 0/-1 */
int sysv_semctl(int id, int semnum, int cmd, int arg);     /* SETVAL/GETVAL/IPC_RMID */
int sysv_sem_format(char *out, int max);                   /* /proc/sysvipc text */

/* System V message queues (M1160): keyed queues of typed messages; msgrcv
 * selects by mtyp (0=oldest, >0=exact type, <0=lowest type <= |mtyp|). */
int sysv_msgget(int key, int flags);
int sysv_msgsnd(int id, long mtype, const void *data, int len, int flags);
int sysv_msgrcv(int id, long mtyp, void *out, int max, long *mtype_out, int flags);

/* System V shared memory (M1161): keyed segments over the M1108 shm backing. */
int      sysv_shmget(int key, uint64_t size, int flags);   /* open/create a segment; id or -1 */
uint64_t sysv_shmat(int id);                               /* attach: map into the caller, base VA or 0 */
