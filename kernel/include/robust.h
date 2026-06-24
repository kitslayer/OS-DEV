/*
 * robust.h — robust-futex shared ABI (M1141), used by the kernel and userspace.
 *
 * A robust mutex's lock word holds the OWNER's thread id (0 = free), optionally
 * OR'd with FUTEX_OWNER_DIED. Each thread registers a `robust_t` (its list of
 * currently-held robust locks) via sys_set_robust_list. If the thread dies while
 * holding locks, the kernel walks that list on exit, sets FUTEX_OWNER_DIED on
 * each still-held word, and wakes a waiter — who then recovers ownership instead
 * of blocking forever. Simplified vs Linux's embedded linked list: a small flat
 * array the thread maintains in userspace (so the lock/unlock fast path stays
 * syscall-free; the kernel only reads it on the rare exit).
 */
#pragma once

#define ROBUST_MAX        8           /* max locks a thread can hold robustly */
#define FUTEX_OWNER_DIED  0x40000000  /* set in the lock word when the owner died */
#define FUTEX_TID_MASK    0x3FFFFFFF  /* the owner-tid bits of the lock word */

typedef struct {
    int   n;                          /* number of held locks (0..ROBUST_MAX) */
    int   _pad;
    void *held[ROBUST_MAX];           /* addresses of the lock words we hold */
} robust_t;
