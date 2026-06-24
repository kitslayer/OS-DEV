/*
 * fifo.h — named pipes (FIFOs), path-keyed (M1188).
 *
 * A FIFO is a pipe (kernel/pipe.c) addressable by a pathname, so UNRELATED
 * processes can rendezvous by name (vs an anonymous pipe, which needs a common
 * fork ancestor). A fixed registry maps path -> a pinned backing pipe; mkfifo()
 * creates the entry, fifo_pipe() resolves a path to its pipe index. The fd layer
 * (struct app.fd / app_fifo_open) hands out read/write ends on top.
 */
#pragma once

int fifo_make(const char *path);    /* create the named FIFO if absent; 0 (exists/created), -1 (full/bad) */
int fifo_pipe(const char *path);    /* -> the FIFO's backing pipe index, or -1 if no such FIFO */
