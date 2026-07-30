/* ipcselftest.h — boot-time POSIX IPC self-test (M1906). See ipcselftest.c.
 *
 * Prints "[ ok ] ipc: ..." markers for message queues, named semaphores, shared
 * memory, ptys, advisory locks, inotify and eventfd, plus a pass/fail summary.
 * Asserted headlessly by tests/run-ipc-tests.sh — kernel kprintf reaches COM1,
 * whereas ring-3 app output does not, which is why this lives in the kernel. */
#pragma once

void ipc_selftest(void);
