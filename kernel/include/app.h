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
int         app_spawn_named_arg(const char *name, const char *arg);  /* launch with a one-shot arg */
int         app_getarg(char *out, int max);      /* read the calling app's launch arg; returns length */
int         app_list_names(char *buf, int max);  /* space-separated prog names; bytes written */
int         app_spawn_from_file(const char *path);/* load + run an ELF from a FAT32 file */
app_t      *app_take_pending(void);              /* next app awaiting a window (WM)    */
void        app_browse(const char *url);         /* queue a URL for a browser window   */
int         app_take_browse(char *out, int max); /* WM claims a queued browse URL; 0/1 */
const char *app_title(app_t *a);
const char *app_arg(app_t *a);          /* the app's launch argument (/proc/<pid>/cmdline) */
void       *app_task(app_t *a);         /* the app's task_t* (cast in procfs for stop/cont) */
uint64_t    app_cr3(app_t *a);          /* the app's CR3 (address space), for /proc/<pid>/wss */
uint64_t    app_heap_bytes(app_t *a);   /* heap size in bytes */
int         app_vma_count(app_t *a);    /* number of mmap regions */
int         app_format_maps(app_t *a, char *buf, int max);   /* /proc/<pid>/maps: memory regions as text */
int    app_alive(app_t *a);
int    app_reap(app_t *a);                       /* free a self-exited app's task+stack+slot (WM) */
void   app_request_kill(app_t *a);               /* ask a running app to close (it self-exits) */
void   app_kill_check(void);                     /* honor a pending kill from a polling/gfx/sleep syscall (exits) */
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
void   app_scroll_frac(app_t *a, int num, int den);  /* scrollbar click/drag: thumb at num/den of track */
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
uint64_t app_mmap(uint64_t len);        /* reserve a demand-paged anonymous region; base VA or 0 */
uint64_t app_ringbuf(uint64_t len);     /* a magic mirrored ring buffer: len frames mapped twice back-to-back */
int      app_mprotect(uint64_t addr, uint64_t len, int prot);   /* change R/W/X of a mapped range (W^X/JIT); 0/-1 */
int      app_munmap(uint64_t addr, uint64_t len);   /* free an mmap region; 0/-1 */
int      app_madvise(uint64_t addr, uint64_t len, int advice);  /* MADV_DONTNEED(4): drop resident anon frames; pages dropped/-1 */
int      app_swap_out(uint64_t addr, uint64_t len);             /* page out anon pages in range to swap; pages/-1 (M1105) */
int      app_fault_handle(uint64_t cr2);            /* #PF hook: lazily map an mmap page; 1 if handled */
struct registers;                                   /* (interrupts.h) */
void app_signal_set(int signo, uint64_t handler, uint64_t restorer);  /* SYS_signal */
int  app_signal_deliver(struct registers *r, int signo);  /* redirect r to the handler; 1 if delivered */
void app_sigreturn(struct registers *r);            /* restore the pre-signal context */
void app_request_signal(app_t *a, int signo);      /* async-raise a signal (Ctrl-C->SIGINT); opt-in (needs a handler) */
int  app_deliver_pending(struct registers *r);     /* deliver a pending async signal on return to ring 3; 1 if delivered */
void app_set_alarm(uint64_t ticks);                /* SYS_alarm: arm a periodic SIGALRM every `ticks` ticks (0=disarm) */
void app_alarm_tick(void);                         /* timer IRQ hook: raise SIGALRM if the current app's alarm is due */
void app_set_traced(app_t *a, int on);             /* strace: log this app's syscalls to dmesg */
int  app_is_traced(app_t *a);
void app_jail_next(uint32_t promises, const char *path);   /* confine the NEXT spawned app (pledge + unveil) */
int    app_gfx_init(int w, int h);     /* put the caller in graphics mode (w*h pixel canvas) */
int    app_gfx_blit(const uint32_t *pixels);  /* copy the caller's pixels to the canvas */
int    app_gfx_get(app_t *a, uint32_t **buf, int *w, int *h);  /* WM: canvas + dims; 1/0 */
void   app_set_rawkb(int on);          /* caller opts into raw make/break key events */
void   app_set_caret(int on);          /* 1 = show system caret (default), 0 = app draws its own */
int    app_caret_hidden(app_t *a);     /* WM: 1 if this app draws its own view (full-screen) */
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
void   app_fault_current(struct registers *r);  /* a ring-3 task faulted: dump a core, kill it, keep the kernel alive; no return */
void   app_core_dump(struct registers *r);      /* write an ET_CORE ELF of the faulting app to /tmp/core (M1104) */

/* pledge() sandbox (M1074): a process voluntarily drops the right to make whole
 * classes of syscalls; the dispatcher kills it if it then tries one. The class
 * bits are the user ABI (named in the string passed to pledge()). */
#define PL_STDIO (1u<<0)   /* basic I/O: read/write/time/sleep/poll/signal/sbrk/rng */
#define PL_RPATH (1u<<1)   /* read the filesystem */
#define PL_WPATH (1u<<2)   /* create / write / delete files */
#define PL_INET  (1u<<3)   /* network */
#define PL_GFX   (1u<<4)   /* graphics + audio + clipboard */
#define PL_PROC  (1u<<5)   /* spawn / kill / process listing */
#define PL_VM    (1u<<6)   /* mmap / munmap */
#define PL_POWER (1u<<7)   /* power off / reboot */

app_t   *app_current(void);             /* the app owning the running task, or NULL */
int      app_pledge(app_t *a, uint32_t mask);   /* restrict promises (monotonic); 0/-1 */
int      app_is_pledged(app_t *a);      /* 1 once pledge() has been called */
uint32_t app_promises(app_t *a);        /* current promise bitmask */
int      app_pledge_parse(const char *s, uint32_t *out);  /* names -> mask; 0 ok, -1 unknown name */
int      app_pledge_format(uint32_t mask, char *buf, int max);  /* mask -> "stdio rpath ..."; bytes */

/* unveil() — restrict which filesystem paths the process can reach (default:
 * all, until the first unveil). A denied access fails (-1), it does not kill. */
#define UV_R (1u<<0)   /* read permission for an unveiled prefix */
#define UV_W (1u<<1)   /* write/create permission */
int      app_unveil(app_t *a, const char *path, uint32_t perms);  /* add a prefix (path NULL = lock); 0/-1 */
uint32_t app_unveil_parse(const char *perms);   /* "rwc" -> UV_* bits */
int      app_unveil_ok(app_t *a, const char *path, int need_write);  /* 1 if the path is reachable */
int    app_sys_history(char *buf, int max);  /* the caller's command history */
