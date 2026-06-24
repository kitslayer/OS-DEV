/*
 * iouring.c — io_uring-lite: drain a userspace submission ring in one trap.
 *
 * See iouring.h for the ABI. The app fills its ring's SQ with a batch of
 * operations and traps in once; we walk sq_head..sq_tail, run each op through
 * the ordinary kernel paths (vfs_read/write, net, the CSPRNG, the timer), and
 * post a CQE per op. One crossing serves a whole batch — fewer ring/ring0
 * transitions, the reason io_uring exists.
 *
 * The ring lives in the calling app's address space, so we touch it directly
 * with its CR3 active. We bounds-check the ring struct and every embedded
 * pointer against the user range (vmm_user_*), and honour unveil() exactly as
 * the standalone read/write syscalls do, so a batch can't reach past what the
 * process is otherwise allowed to.
 */
#include "iouring.h"
#include "vmm.h"
#include "vfs.h"
#include "net.h"
#include "timer.h"
#include "random.h"
#include "app.h"

/* The pledge promise class each op needs (0 = always allowed), so a batch can't
 * reach past the process's pledge() the way the standalone syscalls can't. */
static uint32_t io_op_class(uint32_t op) {
    switch (op) {
    case IO_READFILE:  return PL_RPATH;
    case IO_WRITEFILE: return PL_WPATH;
    case IO_PING:      return PL_INET;
    default:           return PL_STDIO;   /* NOP / SLEEP / GETRANDOM */
    }
}

/* Run one submission entry; returns the result to record in its completion. */
static int64_t io_run_one(const struct io_sqe *s, app_t *self) {
    uint32_t need = io_op_class(s->op);
    if (self && app_is_pledged(self) && need && !(app_promises(self) & need))
        return -1;                          /* this op is outside the process's pledge */

    switch (s->op) {
    case IO_NOP:
        return 0;

    case IO_READFILE:
        if (!vmm_user_str_ok(s->a, 16u << 20) || !vmm_user_ok(s->b, s->c)) return -1;
        if (!app_unveil_ok(self, (const char *)s->a, 0)) return -1;
        return (int64_t)vfs_read((const char *)s->a, (void *)s->b, s->c);

    case IO_WRITEFILE:
        if (!vmm_user_str_ok(s->a, 16u << 20) || !vmm_user_ok(s->b, s->c)) return -1;
        if (!app_unveil_ok(self, (const char *)s->a, 1)) return -1;
        return (int64_t)vfs_write((const char *)s->a, (const void *)s->b, s->c);

    case IO_PING:
        return (int64_t)net_ping_gateway();           /* interrupts already on (see enter) */

    case IO_SLEEP:
        timer_wait(s->a / 10 + 1);
        return 0;

    case IO_GETRANDOM:
        if (s->c == 0 || !vmm_user_ok(s->b, s->c)) return -1;
        random_bytes((void *)s->b, (size_t)s->c);
        return (int64_t)s->c;

    default:
        return -1;                                     /* unknown opcode */
    }
}

long io_uring_enter(uint64_t ring_uptr) {
    if (!vmm_user_ok(ring_uptr, sizeof(struct io_ring))) return -1;
    struct io_ring *r = (struct io_ring *)ring_uptr;
    app_t *self = app_current();

    /* PING and SLEEP block on the timer, so run the batch with interrupts on.
     * Safe: the calling app is parked in this syscall (single CPU), and the
     * ring is just its own memory with its CR3 active. */
    __asm__ volatile("sti");

    long done = 0;
    /* Snapshot the tail once so a buggy/racy app can't make us loop unbounded;
     * cap the batch at the ring depth regardless. */
    uint32_t head = r->sq_head, tail = r->sq_tail;
    for (uint32_t i = 0; head != tail && i < IO_RING_N; head++, i++) {
        struct io_sqe sqe = r->sqe[head % IO_RING_N];   /* copy out before running */
        int64_t res = io_run_one(&sqe, self);

        /* Post the completion if the CQ has room (drop silently if it's full —
         * the app should drain it; res is lost, like a real overflowing CQ). */
        if ((uint32_t)(r->cq_tail - r->cq_head) < IO_RING_N) {
            struct io_cqe *c = &r->cqe[r->cq_tail % IO_RING_N];
            c->user_data = sqe.user_data;
            c->res = res;
            r->cq_tail++;
        }
        done++;
    }
    r->sq_head = head;        /* consume what we processed */
    return done;
}
