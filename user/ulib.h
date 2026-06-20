/* ulib.h — tiny userspace C library (the start of a libc). */
#pragma once

/* raw syscalls */
long sys_write(int fd, const void *buf, unsigned long len);
long sys_read(int fd, void *buf, unsigned long len);
void sys_exit(int code);
int  sys_getpid(void);
long sys_list(void *buf, unsigned long len);
long sys_readfile(const char *name, void *buf, unsigned long len);
long sys_writefile(const char *name, const void *buf, unsigned long len);
long sys_delete(const char *name);
long sys_time(void *buf, unsigned long len);
void sys_beep(int hz, int ms);
long sys_sysinfo(void *buf, unsigned long len);
void sys_clear(void);
void sys_reboot(void);
long sys_ping(void);
long sys_ping_host(const char *host);
long sys_netinfo(void *buf, unsigned long len);
long sys_apps(void *buf, unsigned long len);
long sys_resolve(const char *host, void *buf, unsigned long len);
long sys_http(const char *host, const char *path, void *buf, unsigned long max);
long sys_https(const char *host, const char *path, void *buf, unsigned long max);
long sys_spawn(const char *name);
long sys_browse(const char *url);
long sys_mkdir(const char *path);
long sys_chdir(const char *path);
long sys_tree(void *buf, unsigned long len);
long sys_ps(void *buf, unsigned long len);
long sys_history(void *buf, unsigned long len);
int  sys_pollkey(void);
long sys_df(void *buf, unsigned long len);
long sys_find(const char *want, void *buf, unsigned long len);
long sys_sha256(const char *name, void *hexbuf, unsigned long max);
long sys_sha512(const char *name, void *hexbuf, unsigned long max);
long sys_crypt(const char *name, const char *pass);
long sys_js(const char *src, void *out, unsigned long max);
long sys_screenshot(const char *name);
long sys_gunzip(const char *insrc, const char *outname);
long sys_gzip(const char *insrc, const char *outname);
long sys_unzip(const char *zipname);
long sys_untar(const char *tarname);
void sys_sleep(int ms);
void sys_setcolor(int color);   /* text colour for subsequent output: palette index 0-15 (0 = default) */
void *sbrk(long inc);           /* grow the heap by inc bytes; previous break, or (void*)-1 */
unsigned long sys_uptime_ms(void);   /* monotonic milliseconds since boot */
int  sys_gfx_init(int w, int h);     /* enter graphics mode: a w*h XRGB pixel canvas; 0/-1 */
int  sys_gfx_blit(const void *pixels); /* copy w*h pixels (0x00RRGGBB) to the window; 0/-1 */
void sys_setkbmode(int raw);         /* 1 = raw make/break key events, 0 = cooked ASCII */
void sys_caret(int on);              /* 1 = show system caret (default), 0 = this app draws its own */
int  sys_clip_get(char *buf, int max);     /* copy the system clipboard into buf; returns length */
void sys_clip_set(const char *buf, int len); /* set the system clipboard (shared with middle-click paste) */
int  sys_getarg(char *buf, int max);       /* copy this app's launch argument into buf; returns length */
int  sys_getkbevent(void);           /* next raw key event (scancode|0x100 released|0x200 ext), or -1 */
void sys_pcm(const void *frames, int nframes);   /* play 16-bit stereo PCM @ 48 kHz (blocks) */
long sys_playwav(const char *name);              /* play a .wav file (16-bit PCM); 0/-1 */
int  sys_pcm_stream(const void *frames, int nframes);  /* queue stereo PCM (non-blocking); accepted */
int  sys_pcm_avail(void);                        /* free frames in the streaming ring */
int  sys_mouse(int *x, int *y);   /* cursor relative to the gfx canvas (-1 outside); returns button bits */
void sys_mouse_rel(int *dx, int *dy);   /* accumulated relative motion since last call (mouselook) */
long sys_playbg(const char *name);   /* play a .wav in the background (non-blocking); 0/-1 */
void sys_audiostop(void);            /* stop background audio */

/* dynamic memory (a first-fit free list over sbrk) */
void *malloc(unsigned long n);
void  free(void *p);
void *calloc(unsigned long nmemb, unsigned long size);
void *realloc(void *p, unsigned long n);

/* freestanding mem primitives (GCC may also emit calls to these) */
void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);

/* convenience */
void          print(const char *s);
void          cap_begin(void);                          /* redirect print() into a growable heap buffer */
char         *cap_end(unsigned long *outlen);           /* stop capturing; returns the malloc'd buffer (caller frees) + byte count */
int           readline(char *buf, int max);   /* reads a line, strips '\n', NUL-terminates */
unsigned long ustrlen(const char *s);
int           streq(const char *a, const char *b);
int           startswith(const char *s, const char *prefix);
