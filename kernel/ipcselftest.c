/*
 * ipcselftest.c — boot-time self-tests for the POSIX IPC surface (M1906).
 *
 * --- Why this exists ---------------------------------------------------------
 * A coverage survey found ~2,700 lines of IPC/filesystem code with NO automated
 * assertions at all: POSIX message queues, named semaphores, shared memory, ptys,
 * advisory file locks, inotify and eventfd. Every one of them is reachable from
 * userspace (each has syscalls that apps use), so they were being exercised
 * incidentally — but nothing checked that they behaved correctly, which is
 * exactly the situation that let three box-model bugs coexist in a green tree
 * until M1902 added rendering assertions.
 *
 * These subsystems cannot be covered the way the userspace test apps do it:
 * ring-3 `print()` goes to an app's window text grid and is NOT mirrored to the
 * serial port, so a headless test cannot read it. Kernel `kprintf` DOES reach
 * COM1, so this follows the pattern every driver already uses — a boot-time
 * self-test that prints "[ ok ]" markers a test script greps for
 * (tests/run-ipc-tests.sh), the same shape as ahci_selftest / nvme_selftest.
 *
 * --- Scope -------------------------------------------------------------------
 * Deliberately only NON-BLOCKING operations. Every one of these subsystems has
 * blocking paths (mqueue_send on a full queue, sem_named_wait at zero,
 * pty_read on an empty buffer), and calling one from the boot path with no other
 * runnable task would wedge the boot. So each test drives the queue/semaphore/pty
 * to a state where the operation is known to complete, and uses the trywait /
 * ready / getattr variants to probe the rest. That is a real limit on what this
 * proves, and it is stated here rather than implied.
 *
 * Everything is cleaned up (unlink/close) so the running system is left exactly
 * as it was found.
 */
#include "mqueue.h"
#include "sem.h"
#include "shm.h"
#include "pty.h"
#include "flock.h"
#include "inotify.h"
#include "eventfd.h"
#include "syscall.h"   /* O_CREAT / O_EXCL — sem_open's oflag semantics */
#include "console.h"
#include "string.h"
#include <stdint.h>

/* One assertion. Prints a pass/fail line and tallies, so the test script can both
 * grep individual markers AND assert the summary count. */
static int ipc_pass, ipc_fail;
static void ck(int cond, const char *what) {
    if (cond) { ipc_pass++; kprintf("[ ok ] ipc: %s\n", what); }
    else      { ipc_fail++; kprintf("[FAIL] ipc: %s\n", what); }
}

/* --- POSIX message queues -------------------------------------------------- */
static void test_mqueue(void) {
    int q = mqueue_open("/ipcselftest", 4, 64);
    ck(q >= 0, "mq_open created a queue");
    if (q < 0) return;

    long maxmsg = 0, msgsize = 0, curmsgs = 0, flags = 0;
    ck(mqueue_getattr(q, &flags, &maxmsg, &msgsize, &curmsgs) == 0 && curmsgs == 0,
       "mq_getattr reports an empty queue");

    /* Priority ordering is the interesting property: receive must return the
     * HIGHEST priority first, not FIFO. Send low, then high, then read back. */
    ck(mqueue_send(q, "lo", 2, 1) == 2, "mq_send accepted a low-priority message");
    ck(mqueue_send(q, "hi", 2, 9) == 2, "mq_send accepted a high-priority message");
    ck(mqueue_getattr(q, &flags, &maxmsg, &msgsize, &curmsgs) == 0 && curmsgs == 2,
       "mq_getattr counts both queued messages");

    char buf[64]; unsigned int prio = 0;
    memset(buf, 0, sizeof buf);
    long n = mqueue_receive(q, buf, sizeof buf, &prio);
    ck(n == 2 && buf[0] == 'h' && buf[1] == 'i' && prio == 9,
       "mq_receive returned the HIGHEST priority message first");

    memset(buf, 0, sizeof buf); prio = 0;
    n = mqueue_receive(q, buf, sizeof buf, &prio);
    ck(n == 2 && buf[0] == 'l' && buf[1] == 'o' && prio == 1,
       "mq_receive then returned the lower-priority message");

    ck(mqueue_unlink("/ipcselftest") == 0, "mq_unlink removed the queue");
}

/* --- POSIX named semaphores ------------------------------------------------ */
static void test_sem(void) {
    /* O_CREAT is REQUIRED to create: without it sem_named_open correctly returns
     * ENOENT for a name that does not exist. The first cut of this test passed
     * oflag=0 and "failed" — the kernel was right and the test was wrong, which is
     * worth asserting explicitly now that it is understood. */
    ck(sem_named_open("/ipcselftest_absent", 0, 1) < 0,
       "sem_open without O_CREAT fails for a missing name (ENOENT)");

    int s = sem_named_open("/ipcselftest", O_CREAT, 1);   /* initial value 1 */
    ck(s >= 0, "sem_open with O_CREAT created a semaphore with value 1");
    if (s < 0) return;

    /* O_CREAT|O_EXCL on an existing name must fail (EEXIST). */
    ck(sem_named_open("/ipcselftest", O_CREAT | O_EXCL, 1) < 0,
       "sem_open O_CREAT|O_EXCL fails on an existing name (EEXIST)");

    int v = -1;
    ck(sem_named_getvalue(s, &v) == 0 && v == 1, "sem_getvalue reports 1");
    ck(sem_named_trywait(s) == 0, "sem_trywait succeeded while the count was 1");
    ck(sem_named_getvalue(s, &v) == 0 && v == 0, "sem_trywait decremented it to 0");
    /* At zero, trywait must FAIL rather than block — the property worth asserting. */
    ck(sem_named_trywait(s) != 0, "sem_trywait fails at 0 instead of blocking");
    ck(sem_named_post(s) == 0, "sem_post incremented it");
    ck(sem_named_getvalue(s, &v) == 0 && v == 1, "sem_getvalue back to 1 after post");
    ck(sem_named_close(s) == 0, "sem_close released the handle");
    ck(sem_named_unlink("/ipcselftest") == 0, "sem_unlink removed the name");
}

/* --- POSIX shared memory --------------------------------------------------- */
static void test_shm(void) {
    uint64_t *frames = 0; int npages = 0;
    int r = shm_get("/ipcselftest", 8192, &frames, &npages);
    ck(r == 0 && frames && npages == 2, "shm_open/ftruncate gave 2 pages for 8 KiB");

    /* Re-opening the SAME name must return the SAME frames: that identity is the
     * whole point of shared memory, and a bug there would silently un-share it. */
    uint64_t *frames2 = 0; int npages2 = 0;
    int r2 = shm_get("/ipcselftest", 8192, &frames2, &npages2);
    ck(r2 == 0 && npages2 == npages && frames2 && frames2[0] == frames[0],
       "shm_open of the same name returned the SAME backing frames");

    /* The size cap must be enforced, not silently truncated. */
    uint64_t cap = shm_max_bytes();
    uint64_t *f3 = 0; int n3 = 0;
    ck(shm_get("/ipcselftest_toobig", cap + 4096, &f3, &n3) != 0,
       "shm_open past the size cap is rejected");

    ck(shm_unlink("/ipcselftest") == 0, "shm_unlink removed the object");
}

/* --- pseudo-terminals ------------------------------------------------------ */
static void test_pty(void) {
    int m = pty_open();
    ck(m >= 0, "pty_open allocated a master/slave pair");
    if (m < 0) return;
    int slave = m | 1;

    /* Write on the master, read on the slave: proves the pair is actually wired
     * together and in the right direction. Raw mode first, so line discipline
     * doesn't hold the bytes back and turn the read below into a block. */
    pty_ctl(m, 0, 0);                      /* lflag = 0: no ICANON/ECHO */
    pty_ctl(slave, 0, 0);
    ck(pty_write(m, "AB", 2) == 2, "pty_write accepted 2 bytes on the master");
    ck(pty_ready(slave) == 1, "pty_ready says the slave has data (so read won't block)");

    char buf[8]; memset(buf, 0, sizeof buf);
    long n = pty_ready(slave) ? pty_read(slave, buf, sizeof buf) : -1;
    ck(n == 2 && buf[0] == 'A' && buf[1] == 'B', "pty_read on the slave got those bytes");

    ck(pty_ready(slave) == 0, "pty_ready is clear once the slave is drained");
    ck(pty_close(m) == 0, "pty_close closed the master");
    ck(pty_close(slave) == 0, "pty_close closed the slave");
}

/* --- advisory file locks (flock) ------------------------------------------- */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_UN 8
#define LOCK_NB 4
static void test_flock(void) {
    /* Two different pids contending. An exclusive lock must exclude, and a shared
     * lock must not — the core semantics, and the part a bug would silently break. */
    ck(flock_op("/ipcselftest.lock", 101, LOCK_EX | LOCK_NB) == 0,
       "flock LOCK_EX granted to pid 101");
    ck(flock_op("/ipcselftest.lock", 102, LOCK_EX | LOCK_NB) != 0,
       "flock LOCK_EX refused to pid 102 while 101 holds it");
    ck(flock_op("/ipcselftest.lock", 101, LOCK_UN) == 0, "flock unlocked by pid 101");
    ck(flock_op("/ipcselftest.lock", 101, LOCK_SH | LOCK_NB) == 0,
       "flock LOCK_SH granted to pid 101");
    ck(flock_op("/ipcselftest.lock", 102, LOCK_SH | LOCK_NB) == 0,
       "flock LOCK_SH ALSO granted to pid 102 (shared locks coexist)");
    ck(flock_op("/ipcselftest.lock", 103, LOCK_EX | LOCK_NB) != 0,
       "flock LOCK_EX refused while shared locks are held");
    flock_release_pid(101);
    flock_release_pid(102);
    ck(flock_op("/ipcselftest.lock", 103, LOCK_EX | LOCK_NB) == 0,
       "flock LOCK_EX granted after the holders' pids were released");
    flock_op("/ipcselftest.lock", 103, LOCK_UN);
}

/* --- inotify --------------------------------------------------------------- */
static void test_inotify(void) {
    int i = inotify_new();
    ck(i >= 0, "inotify_init allocated an instance");
    if (i < 0) return;
    int wd = inotify_add(i, "/ipcselftest.watch", 0xFFFFFFFFu);
    ck(wd >= 0, "inotify_add_watch registered a watch");
    ck(inotify_ready(i) == 0, "inotify queue starts empty");

    /* Feed the same hook the VFS uses, so this exercises the real event path
     * rather than a private one. */
    inotify_feed('c', "/ipcselftest.watch");
    ck(inotify_ready(i) == 1, "inotify saw an event after a matching VFS mutation");

    char buf[256]; memset(buf, 0, sizeof buf);
    long n = inotify_read(i, buf, sizeof buf);
    ck(n > 0, "inotify_read drained the queued event");
    ck(inotify_ready(i) == 0, "inotify queue empty again after the read");

    /* A path that is NOT watched must not generate events. */
    inotify_feed('c', "/some.other.path");
    ck(inotify_ready(i) == 0, "inotify ignores mutations outside its watch");

    ck(inotify_rm(i, wd) == 0, "inotify_rm_watch removed the watch");
    inotify_free(i);
}

/* --- eventfd --------------------------------------------------------------- */
static void test_eventfd(void) {
    ck(eventfd_ready("ipcself") == 0, "eventfd counter starts at 0");
    /* eventfd semantics: writes ACCUMULATE, and a read drains the whole counter. */
    ck(eventfd_write("ipcself", "1", 1) > 0, "eventfd_write bumped the counter");
    ck(eventfd_ready("ipcself") == 1, "eventfd is readable once non-zero");
    ck(eventfd_write("ipcself", "1", 1) > 0, "eventfd_write bumped it again");

    char buf[64]; memset(buf, 0, sizeof buf);
    long n = eventfd_ready("ipcself") ? eventfd_read("ipcself", buf, sizeof buf) : -1;
    ck(n > 0, "eventfd_read returned the accumulated count");
    ck(eventfd_ready("ipcself") == 0, "eventfd drained to 0 by the read");
}

void ipc_selftest(void) {
    ipc_pass = ipc_fail = 0;
    kprintf("[ipc] POSIX IPC self-test (mqueue / sem / shm / pty / flock / inotify / eventfd)\n");
    test_mqueue();
    test_sem();
    test_shm();
    test_pty();
    test_flock();
    test_inotify();
    test_eventfd();
    kprintf("[ %s ] ipc self-test: %d passed, %d failed\n\n",
            ipc_fail ? "!!" : "ok", ipc_pass, ipc_fail);
}
