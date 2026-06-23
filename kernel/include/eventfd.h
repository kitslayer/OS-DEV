/*
 * eventfd.h — two kinds of "wait on a file" object, routed by the VFS (M1113):
 *
 *   /timer/<ms>     a sleepable file. read() blocks for <ms> milliseconds, then
 *                   returns "tick\n". The duration is parsed from the path, so a
 *                   shell script can pace itself with `cat /timer/250` — no new
 *                   syscall, just file I/O over the timer wheel (task_sleep_ms).
 *
 *   /event/<name>   an eventfd-style counting semaphore. write a decimal N to add
 *                   N to the counter (and wake a blocked reader); read blocks
 *                   until the counter is non-zero, then returns the count as
 *                   decimal and drains it to 0. Distinct from the mailbox (FIFO
 *                   messages) and notification objects (OR-coalesced bitmask):
 *                   this one ADDS, so it counts events. Like them, the read path
 *                   runs interrupts-off so "zero? then block" + the read-drain are
 *                   atomic against a concurrent writer on this single CPU.
 */
#pragma once
#include <stdint.h>

long timer_read(const char *ms, void *buf, unsigned long max);          /* /timer/<ms>: sleep then "tick\n" */
long eventfd_write(const char *name, const void *data, unsigned long len); /* /event/<name>: counter += N, wake */
long eventfd_read(const char *name, void *buf, unsigned long max);      /* /event/<name>: block until >0, return+drain */
int  eventfd_format(char *out, int max);                               /* /proc/events: live counters */
