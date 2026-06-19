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
void   app_request_kill(app_t *a);               /* ask a running app to close (it self-exits) */
int    app_dirty_clear(app_t *a);                /* 1 if the grid changed (WM poll) */
int    app_sys_pollkey(void);                    /* non-blocking key for the caller */
void   app_render(app_t *a, int px, int py, int focused); /* draw text grid (+caret if focused) */
void   app_key(app_t *a, char c);              /* deliver one keystroke        */

/* Mouse-driven text selection + clipboard (WM calls these; row/col are visible cells). */
void   app_sel_begin(app_t *a, int row, int col);   /* start a selection at the anchor cell */
void   app_sel_extend(app_t *a, int row, int col);  /* drag the selection end */
void   app_sel_commit(app_t *a);                    /* release: copy the selection to the clipboard */
void   app_sel_clear(app_t *a);                     /* drop the highlight */
void   app_sel_word(app_t *a, int row, int col);    /* double-click: select+copy the word at a cell */
void   app_paste(app_t *a);                         /* inject the clipboard into the input queue */
void   clip_set(const char *s, int n);              /* set the system clipboard */
int    clip_get(char *out, int max);                /* read it (NUL-terminated); returns length */
int    app_cols(void);
int    app_rows(void);

/* Called from the syscall dispatcher, acting on the currently-running app. */
void   app_sys_write(const char *buf, unsigned len);
int    app_sys_read(char *buf, unsigned max);
int    app_sys_getpid(void);
uint64_t app_sbrk(long inc);            /* grow the calling app's heap; old break or -1 */
int    app_gfx_init(int w, int h);     /* put the caller in graphics mode (w*h pixel canvas) */
int    app_gfx_blit(const uint32_t *pixels);  /* copy the caller's pixels to the canvas */
int    app_gfx_get(app_t *a, uint32_t **buf, int *w, int *h);  /* WM: canvas + dims; 1/0 */
void   app_set_rawkb(int on);          /* caller opts into raw make/break key events */
void   app_set_caret(int on);          /* 1 = show system caret (default), 0 = app draws its own */
int    app_get_rawkb(app_t *a);        /* WM: is this app in raw keyboard mode? */
void   app_key_raw(app_t *a, unsigned short ev);  /* WM: deliver a raw key event */
int    app_sys_getkbevent(void);       /* next raw key event for the caller, or -1 */
void   app_set_mouse(app_t *a, int x, int y, int btn);  /* WM: canvas-relative cursor + buttons */
long   app_get_mouse(void);            /* SYS_mouse: packed x|y|buttons for the caller */
void   app_add_mouse_rel(app_t *a, int dx, int dy);  /* WM: accumulate relative motion */
long   app_get_mouse_rel(void);        /* SYS_mouse_rel: packed dx|dy, read+cleared */
void   app_sys_clear(void);             /* clear the calling app's screen */
void   app_setcolor(int idx);           /* set the calling app's text colour (palette 0-15) */
void   app_sys_exit(void);              /* does not return */
void   app_fault_current(void);         /* a ring-3 task faulted: kill it, keep the kernel alive; does not return */
int    app_sys_history(char *buf, int max);  /* the caller's command history */
