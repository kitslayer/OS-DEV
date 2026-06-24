/*
 * pipe.h — anonymous pipe objects for the per-process fd table (M1187).
 *
 * A kernel-side table of unidirectional byte pipes, each a ring buffer with a
 * reader-end and writer-end open-count. The per-process fd table (struct app.fd)
 * holds {pipe index, which end}; these calls are the object layer beneath it.
 * Blocking is the mbox.c/unixsock.c discipline (block-once-when-empty/full, woken
 * by the peer; lost-wakeup-free on this single CPU via the IF=0 int-0x80 gate).
 */
#pragma once

int  pipe_new(void);                                          /* -> pipe index (r_open=w_open=1), or -1 */
long pipe_read(int idx, void *buf, unsigned long max);        /* bytes; 0 at EOF (no writers, drained); -1 bad idx */
long pipe_write(int idx, const void *buf, unsigned long len); /* bytes; -1 if no readers (EPIPE) or bad idx */
void pipe_open_end(int idx, int write_end);                   /* fork/dup2: add a reference to one end */
void pipe_close_end(int idx, int write_end);                  /* drop a reference; wake the peer; free at 0/0 */
