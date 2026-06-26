// iouringtest.c — io_uring-lite demo (M1129). Build ONE submission ring, queue
// a batch of four different operations, and cross into the kernel a SINGLE time
// with sys_io_uring_enter(). The kernel runs them in order and posts a
// completion per op. We show that:
//   - one syscall drained four ops (the whole point of io_uring),
//   - ops run IN ORDER: op #2 reads back the file op #1 had just written,
//   - several op types compose (write, read, getrandom) in one batch.
#include "ulib.h"
#include "iouring.h"

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

    // ---- fill the submission queue with a batch of four ops ----
    unsigned t = 0;
    ring.sqe[t++] = (struct io_sqe){ .op = IO_NOP,       .user_data = 0xA0 };
    ring.sqe[t++] = (struct io_sqe){ .op = IO_WRITEFILE, .a = (unsigned long)"/tmp/iou.txt",
                                     .b = (unsigned long)msg, .c = ustrlen(msg), .user_data = 0xA1 };
    ring.sqe[t++] = (struct io_sqe){ .op = IO_READFILE,  .a = (unsigned long)"/tmp/iou.txt",
                                     .b = (unsigned long)rdbuf, .c = sizeof rdbuf - 1, .user_data = 0xA2 };
    ring.sqe[t++] = (struct io_sqe){ .op = IO_GETRANDOM, .b = (unsigned long)rnd, .c = sizeof rnd, .user_data = 0xA3 };
    ring.sq_tail = t;                  // publish the batch

    sys_setcolor(4); print("io_uring:"); sys_setcolor(0); print(" queued ");  pdec(t);  print(" ops; one sys_io_uring_enter()...\n\n");

    long done = sys_io_uring_enter(&ring);

    print("enter() processed ");  pdec(done);  print(" op(s); completions:\n");
    while (ring.cq_head != ring.cq_tail) {
        struct io_cqe *c = &ring.cqe[ring.cq_head % IO_RING_N];
        print("  cqe tag=0x"); phex((unsigned char *)&c->user_data, 1);
        print(" res="); pdec(c->res); print("\n");
        ring.cq_head++;
    }

    long n = 0;
    for (long i = 0; i < (long)sizeof rdbuf && rdbuf[i]; i++) n = i + 1;
    rdbuf[n] = 0;
    print("\nop#2 read back what op#1 wrote (in-order): \"");  print(rdbuf);  print("\"\n");
    print("op#3 getrandom: ");  phex(rnd, sizeof rnd);  print("\n");
    sys_setcolor(9); print("\nfour ops, one ring0 crossing. that's io_uring.\n"); sys_setcolor(0);

    sys_sleep(20000);
    return 0;
}
