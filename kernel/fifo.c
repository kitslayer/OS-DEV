/*
 * fifo.c — named pipes (FIFOs), path-keyed (M1188). See fifo.h.
 *
 * A small fixed registry of {path -> pinned backing pipe}. mkfifo() allocates a
 * pinned, unopened pipe (pipe_new_fifo) and records the name; fifo_pipe() looks
 * it up. The fd layer (app_fifo_open) opens read/write ends on the pipe, and the
 * pinned pipe persists across opens/closes so the named FIFO outlives any single
 * connection — exactly the POSIX FIFO lifetime. Path-keyed like flock.c.
 */
#include "fifo.h"
#include "pipe.h"

#define NFIFO   16
#define FIFO_PATH 64

struct fifo { int used; int pipe; char path[FIFO_PATH]; };
static struct fifo fifos[NFIFO];

static int peq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int fifo_pipe(const char *path) {
    for (int i = 0; i < NFIFO; i++) if (fifos[i].used && peq(fifos[i].path, path)) return fifos[i].pipe;
    return -1;
}

int fifo_make(const char *path) {
    if (!path || !path[0]) return -1;
    /* bound the name */
    int n = 0; while (path[n]) { if (n >= FIFO_PATH - 1) return -1; n++; }
    if (fifo_pipe(path) >= 0) return 0;                  /* already a FIFO -> idempotent */
    int slot = -1;
    for (int i = 0; i < NFIFO; i++) if (!fifos[i].used) { slot = i; break; }
    if (slot < 0) return -1;                             /* registry full */
    int idx = pipe_new_fifo();
    if (idx < 0) return -1;                              /* no free pipe */
    fifos[slot].used = 1; fifos[slot].pipe = idx;
    for (int i = 0; i <= n; i++) fifos[slot].path[i] = path[i];
    return 0;
}
