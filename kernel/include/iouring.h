/*
 * iouring.h — io_uring-lite: a batched async-I/O submission/completion ring,
 * shared by the kernel and userspace (M1129).
 *
 * Ordinary syscalls cost one trap per operation. io_uring amortizes that: an
 * app builds a ring in its own memory, fills the SUBMISSION queue (SQ) with a
 * batch of operations, and crosses into the kernel ONCE (sys_io_uring_enter).
 * The kernel walks the SQ from sq_head to sq_tail, runs each op by reusing the
 * ordinary VFS/net paths, and posts a result to the COMPLETION queue (CQ). One
 * trap drains a whole batch — the essence of Linux's io_uring, in miniature.
 *
 * The ring lives in the app's address space, so the kernel reads SQEs and
 * writes CQEs directly while the app's CR3 is active (no shared frame needed);
 * the whole struct is bounds-checked against the user range on entry.
 */
#pragma once
#include <stdint.h>

#define IO_RING_N 16        /* ring depth: the SQ and CQ each hold this many */

/* opcodes — the arg columns name how a, b, c are used */
enum {
    IO_NOP = 0,             /* (—)                  -> 0                       */
    IO_READFILE,            /* a=path b=buf c=len   -> bytes read,    or -1    */
    IO_WRITEFILE,           /* a=path b=buf c=len   -> bytes written, or -1    */
    IO_PING,                /* (—)                  -> gateway echo replies/-1 */
    IO_SLEEP,               /* a=milliseconds       -> 0                       */
    IO_GETRANDOM,           /* b=buf c=len          -> bytes written           */
    IO_OPMAX
};

struct io_sqe {             /* submission-queue entry */
    uint32_t op;            /* one of IO_*                       */
    uint32_t _pad;
    uint64_t a, b, c;       /* operands (see the opcode table)   */
    uint64_t user_data;     /* opaque tag, echoed back in the CQE */
};

struct io_cqe {             /* completion-queue entry */
    uint64_t user_data;     /* the originating SQE's tag */
    int64_t  res;           /* the operation's result    */
};

struct io_ring {
    volatile uint32_t sq_head, sq_tail;   /* app advances sq_tail; kernel advances sq_head */
    volatile uint32_t cq_head, cq_tail;   /* kernel advances cq_tail; app advances cq_head */
    uint32_t _resv[4];
    struct io_sqe sqe[IO_RING_N];
    struct io_cqe cqe[IO_RING_N];
};

/* Drain the submission queue: run each pending SQE and post its completion.
 * Returns the number of SQEs processed, or -1 on a bad ring pointer. Defined in
 * kernel/iouring.c; harmlessly unreferenced in userspace. */
long io_uring_enter(uint64_t ring_uptr);
