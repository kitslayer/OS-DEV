/*
 * app.h — userspace applications running as windowed processes.
 *
 * An "app" is a real ring-3 program (the embedded shell ELF) running in its own
 * address space as a preemptive task. Its stdout goes to a text grid the window
 * manager draws; its stdin comes from the keyboard when its window is focused.
 */
#pragma once
#include <stdint.h>

typedef struct app app_t;

app_t      *app_spawn(const void *elf, const char *title, uint64_t elfsz);  /* run an ELF as a process */
int         app_spawn_named(const char *name);   /* launch a registered program; 0/-1 */
int         app_list_names(char *buf, int max);  /* space-separated prog names; bytes written */
int         app_spawn_from_file(const char *path);/* load + run an ELF from a FAT32 file */
app_t      *app_take_pending(void);              /* next app awaiting a window (WM)    */
void        app_browse(const char *url);         /* queue a URL for a browser window   */
int         app_take_browse(char *out, int max); /* WM claims a queued browse URL; 0/1 */
const char *app_title(app_t *a);
int    app_alive(app_t *a);
int    app_reap(app_t *a);                       /* free a self-exited app's task+stack+slot (WM) */
int    app_dirty_clear(app_t *a);                /* 1 if the grid changed (WM poll) */
int    app_sys_pollkey(void);                    /* non-blocking key for the caller */
void   app_render(app_t *a, int px, int py);   /* draw the app's text grid    */
void   app_key(app_t *a, char c);              /* deliver one keystroke        */
int    app_cols(void);
int    app_rows(void);

/* Called from the syscall dispatcher, acting on the currently-running app. */
void   app_sys_write(const char *buf, unsigned len);
int    app_sys_read(char *buf, unsigned max);
int    app_sys_getpid(void);
void   app_sys_clear(void);             /* clear the calling app's screen */
void   app_setcolor(int idx);           /* set the calling app's text colour (palette 0-15) */
void   app_sys_exit(void);              /* does not return */
void   app_fault_current(void);         /* a ring-3 task faulted: kill it, keep the kernel alive; does not return */
int    app_sys_history(char *buf, int max);  /* the caller's command history */
