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
void          cap_begin(char *buf, unsigned long max);  /* redirect print() into buf (length-capped, NUL-terminated) */
unsigned long cap_end(void);                            /* stop capturing; returns captured byte count */
int           readline(char *buf, int max);   /* reads a line, strips '\n', NUL-terminates */
unsigned long ustrlen(const char *s);
int           streq(const char *a, const char *b);
int           startswith(const char *s, const char *prefix);
