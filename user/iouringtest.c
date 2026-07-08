// iouringtest.c — io_uring-lite demo (M1129). Build ONE submission ring, queue
// a batch of four different operations, and cross into the kernel a SINGLE time
// with sys_io_uring_enter(). The kernel runs them in order and posts a
// completion per op. We show that:
//   - one syscall drained four ops (the whole point of io_uring),
//   - ops run IN ORDER: op #2 reads back the file op #1 had just written,
//   - several op types compose (write, read, getrandom) in one batch.
#include "ulib.h"
#include "iouring.h"

#define TAG_UNSEEN -12345              /* out-of-band: a real completion res is never this */

static void pdec(long v) {            // print a signed decimal
    char b[24]; int i = 0, neg = v < 0;
    unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    if (!u) b[i++] = '0';
    while (u) { b[i++] = '0' + (u % 10); u /= 10; }
    if (neg) b[i++] = '-';
    char o[24]; int j = 0;
    while (i) o[j++] = b[--i];
    o[j] = 0; print(o);
}
static void phex(const unsigned char *p, int n) {   // print n bytes as hex
    const char *h = "0123456789abcdef";
    char o[3]; o[2] = 0;
    for (int i = 0; i < n; i++) { o[0] = h[p[i] >> 4]; o[1] = h[p[i] & 15]; print(o); }
}

static struct io_ring ring;           // in BSS: user memory the kernel reads/writes directly

int main(void) {
    const char *msg = "hello from a batched io_uring write";
    static char rdbuf[128];
    static unsigned char rnd[16];
    static char pipebuf[8];
    int fds[2]; int have_pipe = (sys_pipe(fds) == 0);

    // ---- fill the submission queue with a batch of ops ----
    unsigned t = 0;
    ring.sqe[t++] = (struct io_sqe){ .op = IO_NOP,       .user_data = 0xA0 };
    ring.sqe[t++] = (struct io_sqe){ .op = IO_WRITEFILE, .a = (unsigned long)"/tmp/iou.txt",
                                     .b = (unsigned long)msg, .c = ustrlen(msg), .user_data = 0xA1 };
    ring.sqe[t++] = (struct io_sqe){ .op = IO_READFILE,  .a = (unsigned long)"/tmp/iou.txt",
                                     .b = (unsigned long)rdbuf, .c = sizeof rdbuf - 1, .user_data = 0xA2 };
    ring.sqe[t++] = (struct io_sqe){ .op = IO_GETRANDOM, .b = (unsigned long)rnd, .c = sizeof rnd, .user_data = 0xA3 };
    // IO_READ/IO_WRITE (M1546): the actual point of io_uring -- batching ops
    // against an fd you already hold, not just whole-file-by-path. In order,
    // in the SAME batch: write "pipe!" into the pipe, then read it back out.
    if (have_pipe) {
        ring.sqe[t++] = (struct io_sqe){ .op = IO_WRITE, .a = (unsigned long)fds[1],
                                         .b = (unsigned long)"pipe!", .c = 5, .user_data = 0xA4 };
        ring.sqe[t++] = (struct io_sqe){ .op = IO_READ,  .a = (unsigned long)fds[0],
                                         .b = (unsigned long)pipebuf, .c = 5, .user_data = 0xA5 };
    }
    // IOSQE_IO_LINK (M1552): a failing linked op cancels the rest of ITS
    // chain, but ops after the chain still run normally. Chain: a write that
    // succeeds (linked) -> a read from a bad fd that fails (linked) -> a
    // read from a perfectly valid fd, which must still be CANCELLED because
    // the op before it in the chain failed (linked) -> a NOP, the chain's
    // un-linked terminator, also cancelled -> one final, independent NOP
    // that must run normally, proving the cancellation didn't leak past the chain.
    static char linkbuf[8];
    if (have_pipe) {
        ring.sqe[t++] = (struct io_sqe){ .op = IO_WRITE, .flags = IOSQE_IO_LINK, .a = (unsigned long)fds[1],
                                         .b = (unsigned long)"link!", .c = 5, .user_data = 0xB0 };
        ring.sqe[t++] = (struct io_sqe){ .op = IO_READ,  .flags = IOSQE_IO_LINK, .a = 999,   /* bad fd -> deliberate failure */
                                         .b = (unsigned long)linkbuf, .c = 5, .user_data = 0xB1 };
        ring.sqe[t++] = (struct io_sqe){ .op = IO_READ,  .flags = IOSQE_IO_LINK, .a = (unsigned long)fds[0],
                                         .b = (unsigned long)linkbuf, .c = 5, .user_data = 0xB2 };
        ring.sqe[t++] = (struct io_sqe){ .op = IO_NOP,   .user_data = 0xB3 };   /* chain terminator: no LINK flag, still cancelled */
        ring.sqe[t++] = (struct io_sqe){ .op = IO_NOP,   .user_data = 0xB4 };   /* independent: must run normally */
    }
    ring.sq_tail = t;                  // publish the batch

    sys_setcolor(4); print("io_uring:"); sys_setcolor(0); print(" queued ");  pdec(t);  print(" ops; one sys_io_uring_enter()...\n\n");

    long done = sys_io_uring_enter(&ring);

    print("enter() processed ");  pdec(done);  print(" op(s); completions:\n");
    long res_by_tag[256]; for (int i = 0; i < 256; i++) res_by_tag[i] = TAG_UNSEEN;   /* sentinel: tag never seen */
    while (ring.cq_head != ring.cq_tail) {
        struct io_cqe *c = &ring.cqe[ring.cq_head % IO_RING_N];
        print("  cqe tag=0x"); phex((unsigned char *)&c->user_data, 1);
        print(" res="); pdec(c->res); print("\n");
        if (c->user_data < 256) res_by_tag[c->user_data] = c->res;
        ring.cq_head++;
    }

    long n = 0;
    for (long i = 0; i < (long)sizeof rdbuf && rdbuf[i]; i++) n = i + 1;
    rdbuf[n] = 0;
    print("\nop#2 read back what op#1 wrote (in-order): \"");  print(rdbuf);  print("\"\n");
    print("op#3 getrandom: ");  phex(rnd, sizeof rnd);  print("\n");
    if (have_pipe) {
        int pok = (pipebuf[0]=='p' && pipebuf[1]=='i' && pipebuf[2]=='p' && pipebuf[3]=='e' && pipebuf[4]=='!');
        print("op#4/#5 IO_WRITE+IO_READ on a pipe fd, same batch: ");
        sys_setcolor(pok ? 10 : 4); print(pok ? "wrote+read back \"pipe!\" through the fd -- OK\n" : "MISMATCH\n"); sys_setcolor(0);

        /* TAG_UNSEEN is itself negative, so a "< 0" check alone can't tell a genuinely-failed/
         * cancelled op apart from one whose completion never got posted at all -- exactly the
         * failure mode these assertions exist to catch. Exclude the sentinel explicitly. */
        int link_ok = (res_by_tag[0xB0] == 5)     // the write at the head of the chain succeeded
                   && (res_by_tag[0xB1] < 0 && res_by_tag[0xB1] != TAG_UNSEEN)   // the bad-fd read genuinely failed
                   && (res_by_tag[0xB2] < 0 && res_by_tag[0xB2] != TAG_UNSEEN)   // cancelled: a VALID fd, but linked after the failure
                   && (res_by_tag[0xB3] < 0 && res_by_tag[0xB3] != TAG_UNSEEN)   // cancelled: the chain's un-linked terminator
                   && (res_by_tag[0xB4] == 0);    // independent NOP after the chain: ran normally
        print("IOSQE_IO_LINK: a failing linked op cancels the rest of its chain: ");
        sys_setcolor(link_ok ? 10 : 4); print(link_ok ? "OK\n" : "VERIFY FAILED\n"); sys_setcolor(0);

        sys_fdclose(fds[0]); sys_fdclose(fds[1]);
    }
    sys_setcolor(9); print("\n");  pdec((long)t);  print(" ops, one ring0 crossing. that's io_uring.\n"); sys_setcolor(0);

    sys_sleep(20000);
    return 0;
}
