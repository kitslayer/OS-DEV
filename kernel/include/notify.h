/*
 * notify.h — named notification objects (seL4-style coalescing doorbells).
 *
 * The signalling counterpart to the mailbox's data queue (mbox.c): each named
 * object holds a single OR-accumulated bitmask. A writer ORs bits in (a "signal"
 * edge); a blocked reader wakes and atomically reads-and-clears the accumulated
 * mask — so many signals that arrive before a read COALESCE into one wakeup
 * carrying the union of their bits. The VFS routes /notify/<name> here. Like the
 * mailbox, the read path runs interrupts-off, so "no bits? then block" and the
 * read-and-clear are atomic against a writer on this single CPU (no lost wakeup).
 */
#pragma once
#include <stdint.h>

/* write /notify/<name>: parse a decimal bitmask from the data (0/empty => bit 0),
 * OR it into the object, and wake any blocked waiter. Returns bytes consumed. */
long notify_signal(const char *name, const void *data, unsigned long len);

/* read /notify/<name>: block until the mask is non-zero, then return it as a
 * decimal string and clear it (read-and-clear). A stray wake with no bits set
 * returns 0 rather than re-blocking. */
long notify_wait(const char *name, void *buf, unsigned long max);

int  notify_format(char *out, int max);   /* /proc/notify: objects + pending masks */
int  notify_ready(const char *name);      /* fswait peek: any bits pending? (M1125) */
