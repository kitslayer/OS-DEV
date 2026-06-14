/*
 * ulib.c — the userspace runtime + a minimal libc.
 *
 * This is the beginning of a real libc: program startup (`_start` → `main`),
 * system-call wrappers, and a few string/IO helpers. It links into every user
 * program. Everything ultimately goes through `int 0x80`.
 */
#include "ulib.h"
#include "syscall.h"

static long do_syscall(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "memory");
    return ret;
}

long sys_write(int fd, const void *buf, unsigned long len) {
    return do_syscall(SYS_write, fd, (long)buf, (long)len);
}
long sys_read(int fd, void *buf, unsigned long len) {
    return do_syscall(SYS_read, fd, (long)buf, (long)len);
}
void sys_exit(int code) { do_syscall(SYS_exit, code, 0, 0); }
int  sys_getpid(void)   { return (int)do_syscall(SYS_getpid, 0, 0, 0); }
long sys_list(void *buf, unsigned long len) {
    /* leading 0 so buf/len land in the same registers (rsi/rdx) the kernel
     * reads them from — matching the write/readfile arg layout. */
    return do_syscall(SYS_list, 0, (long)buf, (long)len);
}
long sys_readfile(const char *name, void *buf, unsigned long len) {
    return do_syscall(SYS_readfile, (long)name, (long)buf, (long)len);
}
long sys_writefile(const char *name, const void *buf, unsigned long len) {
    return do_syscall(SYS_writefile, (long)name, (long)buf, (long)len);
}
long sys_delete(const char *name) { return do_syscall(SYS_delete, (long)name, 0, 0); }
long sys_time(void *buf, unsigned long len) {
    return do_syscall(SYS_time, 0, (long)buf, (long)len);
}
void sys_beep(int hz, int ms) { do_syscall(SYS_beep, hz, ms, 0); }
long sys_sysinfo(void *buf, unsigned long len) { return do_syscall(SYS_sysinfo, 0, (long)buf, (long)len); }
void sys_clear(void)  { do_syscall(SYS_clear, 0, 0, 0); }
void sys_reboot(void) { do_syscall(SYS_reboot, 0, 0, 0); }
long sys_ping(void) { return do_syscall(SYS_ping, 0, 0, 0); }
long sys_spawn(const char *name) { return do_syscall(SYS_spawn, (long)name, 0, 0); }
long sys_browse(const char *url) { return do_syscall(SYS_browse, (long)url, 0, 0); }
long sys_mkdir(const char *path) { return do_syscall(SYS_mkdir, (long)path, 0, 0); }
long sys_chdir(const char *path) { return do_syscall(SYS_chdir, (long)path, 0, 0); }
long sys_tree(void *buf, unsigned long len) { return do_syscall(SYS_tree, 0, (long)buf, (long)len); }
long sys_ps(void *buf, unsigned long len) { return do_syscall(SYS_ps, 0, (long)buf, (long)len); }
long sys_history(void *buf, unsigned long len) { return do_syscall(SYS_history, 0, (long)buf, (long)len); }
int  sys_pollkey(void) { return (int)do_syscall(SYS_pollkey, 0, 0, 0); }
long sys_df(void *buf, unsigned long len) { return do_syscall(SYS_df, 0, (long)buf, (long)len); }
long sys_find(const char *want, void *buf, unsigned long len) { return do_syscall(SYS_find, (long)want, (long)buf, (long)len); }
long sys_sha256(const char *name, void *hexbuf, unsigned long max) { return do_syscall(SYS_sha256, (long)name, (long)hexbuf, (long)max); }
long sys_crypt(const char *name, const char *pass) { return do_syscall(SYS_crypt, (long)name, (long)pass, 0); }
long sys_js(const char *src, void *out, unsigned long max) { return do_syscall(SYS_js, (long)src, (long)out, (long)max); }
void sys_sleep(int ms) { do_syscall(SYS_sleep, ms, 0, 0); }
long sys_resolve(const char *host, void *buf, unsigned long len) {
    return do_syscall(SYS_resolve, (long)host, (long)buf, (long)len);
}
long sys_http(const char *host, const char *path, void *buf, unsigned long max) {
    long ret;
    register long r10 __asm__("r10") = (long)max;       /* 4th arg via r10 */
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"((long)SYS_http), "D"((long)host), "S"((long)path),
                       "d"((long)buf), "r"(r10)
                     : "memory");
    return ret;
}
long sys_https(const char *host, const char *path, void *buf, unsigned long max) {
    long ret;
    register long r10 __asm__("r10") = (long)max;       /* 4th arg via r10 */
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"((long)SYS_https), "D"((long)host), "S"((long)path),
                       "d"((long)buf), "r"(r10)
                     : "memory");
    return ret;
}

unsigned long ustrlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

void print(const char *s) { sys_write(1, s, ustrlen(s)); }

int readline(char *buf, int max) {
    long n = sys_read(0, buf, (unsigned long)(max - 1));
    if (n > 0 && buf[n - 1] == '\n')
        n--;                       /* drop the trailing newline */
    buf[n] = '\0';
    return (int)n;
}

int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

int startswith(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}

/* Program entry: the ELF entry point. Calls main() and exits with its result. */
extern int main(void);
void _start(void) {
    sys_exit(main());
    for (;;) { }
}
