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
void sys_setcolor(int color) { do_syscall(SYS_setcolor, color, 0, 0); }
void sys_reboot(void) { do_syscall(SYS_reboot, 0, 0, 0); }
void sys_poweroff(void) { do_syscall(SYS_poweroff, 0, 0, 0); }
long sys_kill(int pid) { return do_syscall(SYS_kill, pid, 0, 0); }
long sys_ping(void) { return do_syscall(SYS_ping, 0, 0, 0); }
long sys_ping_host(const char *host) { return do_syscall(SYS_pinghost, (long)host, 0, 0); }
long sys_netinfo(void *buf, unsigned long len) { return do_syscall(SYS_netinfo, (long)buf, (long)len, 0); }
long sys_dhcp(void) { return do_syscall(SYS_dhcp, 0, 0, 0); }
long sys_cas_store(const void *buf, unsigned long len, void *hash32) { return do_syscall(SYS_cas_store, (long)buf, (long)len, (long)hash32); }
long sys_cas_fetch(const void *hash32, void *buf, unsigned long max) { return do_syscall(SYS_cas_fetch, (long)hash32, (long)buf, (long)max); }
long sys_tftp(const char *filename, void *buf, unsigned long max) { return do_syscall(SYS_tftp, (long)filename, (long)buf, (long)max); }
long sys_madvise(void *addr, unsigned long len, int advice) { return do_syscall(SYS_madvise, (long)addr, (long)len, advice); }
long sys_alarm(unsigned long ticks) { return do_syscall(SYS_alarm, (long)ticks, 0, 0); }
long sys_sntp(void) { return do_syscall(SYS_sntp, 0, 0, 0); }
long sys_swapout(void *addr, unsigned long len) { return do_syscall(SYS_swapout, (long)addr, (long)len, 0); }
long sys_losetup(const char *path) { return do_syscall(SYS_losetup, (long)path, 0, 0); }
void *sys_shm_open(const char *name, unsigned long size) { return (void *)do_syscall(SYS_shm_open, (long)name, (long)size, 0); }
long sys_futex(void *uaddr, int op, int val) { return do_syscall(SYS_futex, (long)uaddr, op, val); }
long sys_apps(void *buf, unsigned long len) { return do_syscall(SYS_apps, (long)buf, (long)len, 0); }
long sys_spawn(const char *name) { return do_syscall(SYS_spawn, (long)name, 0, 0); }
long sys_spawn_arg(const char *name, const char *arg) { return do_syscall(SYS_spawn, (long)name, (long)arg, 0); }
long sys_browse(const char *url) { return do_syscall(SYS_browse, (long)url, 0, 0); }
long sys_mkdir(const char *path) { return do_syscall(SYS_mkdir, (long)path, 0, 0); }
long sys_chdir(const char *path) { return do_syscall(SYS_chdir, (long)path, 0, 0); }
long sys_tree(void *buf, unsigned long len) { return do_syscall(SYS_tree, 0, (long)buf, (long)len); }
long sys_ps(void *buf, unsigned long len) { return do_syscall(SYS_ps, 0, (long)buf, (long)len); }
long sys_history(void *buf, unsigned long len) { return do_syscall(SYS_history, 0, (long)buf, (long)len); }
int  sys_pollkey(void) { return (int)do_syscall(SYS_pollkey, 0, 0, 0); }
long sys_df(void *buf, unsigned long len) { return do_syscall(SYS_df, 0, (long)buf, (long)len); }
long sys_lspci(void *buf, unsigned long len) { return do_syscall(SYS_lspci, (long)buf, (long)len, 0); }
long sys_lsblk(void *buf, unsigned long len) { return do_syscall(SYS_lsblk, (long)buf, (long)len, 0); }
long sys_mounts(void *buf, unsigned long len) { return do_syscall(SYS_mounts, (long)buf, (long)len, 0); }
long sys_getrandom(void *buf, unsigned long len) { return do_syscall(SYS_getrandom, (long)buf, (long)len, 0); }
int  sys_pledge(const char *promises) { return (int)do_syscall(SYS_pledge, (long)promises, 0, 0); }
int  sys_unveil(const char *path, const char *perms) { return (int)do_syscall(SYS_unveil, (long)path, (long)perms, 0); }
int  sys_symlink(const char *linkpath, const char *target) { return (int)do_syscall(SYS_symlink, (long)linkpath, (long)target, 0); }
int  sys_jail(const char *prog, const char *promises, const char *path) { return (int)do_syscall(SYS_jail, (long)prog, (long)promises, (long)path); }
long sys_find(const char *want, void *buf, unsigned long len) { return do_syscall(SYS_find, (long)want, (long)buf, (long)len); }
long sys_sha256(const char *name, void *hexbuf, unsigned long max) { return do_syscall(SYS_sha256, (long)name, (long)hexbuf, (long)max); }
long sys_sha512(const char *name, void *hexbuf, unsigned long max) { return do_syscall(SYS_sha512, (long)name, (long)hexbuf, (long)max); }
long sys_crypt(const char *name, const char *pass) { return do_syscall(SYS_crypt, (long)name, (long)pass, 0); }
long sys_js(const char *src, void *out, unsigned long max) { return do_syscall(SYS_js, (long)src, (long)out, (long)max); }
long sys_screenshot(const char *name) { return do_syscall(SYS_screenshot, (long)name, 0, 0); }
long sys_setwall(const char *name) { return do_syscall(SYS_setwall, (long)name, 0, 0); }
long sys_savebmp(const char *name, const void *pixels, int w, int h) {
    long ret;
    register long r10 __asm__("r10") = (long)h;         /* 4th arg via r10 */
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"((long)SYS_savebmp), "D"((long)name), "S"((long)pixels),
                       "d"((long)w), "r"(r10)
                     : "memory");
    return ret;
}
long sys_gunzip(const char *insrc, const char *outname) { return do_syscall(SYS_gunzip, (long)insrc, (long)outname, 0); }
long sys_gzip(const char *insrc, const char *outname) { return do_syscall(SYS_gzip, (long)insrc, (long)outname, 0); }
long sys_unzip(const char *zipname) { return do_syscall(SYS_unzip, (long)zipname, 0, 0); }
long sys_untar(const char *tarname) { return do_syscall(SYS_untar, (long)tarname, 0, 0); }
void sys_sleep(int ms) { do_syscall(SYS_sleep, ms, 0, 0); }
void *sbrk(long inc) { return (void *)do_syscall(SYS_sbrk, inc, 0, 0); }
void *sys_mmap(unsigned long len) { long r = do_syscall(SYS_mmap, (long)len, 0, 0); return r ? (void *)r : 0; }
void *sys_ringbuf(unsigned long len) { long r = do_syscall(SYS_ringbuf, (long)len, 0, 0); return r ? (void *)r : 0; }
int   sys_mprotect(void *addr, unsigned long len, int prot) { return (int)do_syscall(SYS_mprotect, (long)addr, (long)len, prot); }
int   sys_bind(const char *from, const char *to) { return (int)do_syscall(SYS_bind, (long)from, (long)to, 0); }
long  sys_munmap(void *addr, unsigned long len) { return do_syscall(SYS_munmap, (long)addr, (long)len, 0); }
/* Restorer trampoline: a signal handler returns HERE; we ask the kernel to
 * restore the pre-signal context (which iretq's elsewhere, so this never
 * returns). The kernel is told this address via sys_signal's 3rd arg. */
void sig_trampoline(void) { do_syscall(SYS_sigreturn, 0, 0, 0); for (;;) { } }
long sys_signal(int signo, void (*handler)(int)) { return do_syscall(SYS_signal, signo, (long)handler, (long)sig_trampoline); }
void sys_raise(int signo) { do_syscall(SYS_raise, signo, 0, 0); }
unsigned long sys_uptime_ms(void) { return (unsigned long)do_syscall(SYS_uptime_ms, 0, 0, 0); }
int  sys_gfx_init(int w, int h) { return (int)do_syscall(SYS_gfx_init, w, h, 0); }
int  sys_gfx_blit(const void *pixels) { return (int)do_syscall(SYS_gfx_blit, (long)pixels, 0, 0); }
void sys_setkbmode(int raw) { do_syscall(SYS_setkbmode, raw, 0, 0); }
void sys_caret(int on) { do_syscall(SYS_caret, on, 0, 0); }
int  sys_clip_get(char *buf, int max) { return (int)do_syscall(SYS_clip_get, (long)buf, max, 0); }
void sys_clip_set(const char *buf, int len) { do_syscall(SYS_clip_set, (long)buf, len, 0); }
int  sys_getarg(char *buf, int max) { return (int)do_syscall(SYS_getarg, (long)buf, max, 0); }
int  sys_getkbevent(void) { return (int)do_syscall(SYS_getkbevent, 0, 0, 0); }
void sys_pcm(const void *frames, int nframes) { do_syscall(SYS_pcm, (long)frames, nframes, 0); }
long sys_playwav(const char *name) { return do_syscall(SYS_playwav, (long)name, 0, 0); }
int  sys_pcm_stream(const void *frames, int nframes) { return (int)do_syscall(SYS_pcm_stream, (long)frames, nframes, 0); }
int  sys_pcm_avail(void) { return (int)do_syscall(SYS_pcm_avail, 0, 0, 0); }
int  sys_mouse(int *x, int *y) {
    long v = do_syscall(SYS_mouse, 0, 0, 0);
    if (x) *x = (int)(short)(v & 0xFFFF);          /* sign-extend 16-bit (-1 = outside) */
    if (y) *y = (int)(short)((v >> 16) & 0xFFFF);
    return (int)((v >> 32) & 0x7);
}
void sys_mouse_rel(int *dx, int *dy) {
    long v = do_syscall(SYS_mouse_rel, 0, 0, 0);
    if (dx) *dx = (int)(v & 0xFFFFFFFF);
    if (dy) *dy = (int)((v >> 32) & 0xFFFFFFFF);
}
long sys_playbg(const char *name) { return do_syscall(SYS_playbg, (long)name, 0, 0); }
void sys_audiostop(void) { do_syscall(SYS_audiostop, 0, 0, 0); }
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

/* Output capture: when a capture buffer is installed (via cap_begin), print()
 * appends to it instead of writing to fd 1. Used by the shell to grab one
 * command's output and feed it to the next stage of a pipe. Off by default —
 * g_capbuf is NULL for every other program and the whole boot, so print() is
 * byte-for-byte the plain sys_write path until a caller opts in. */
static char         *g_capbuf = 0;     /* destination buffer, or NULL = not capturing */
static unsigned long  g_caplen = 0;    /* bytes written so far */
static unsigned long  g_capmax = 0;    /* buffer capacity (incl. room for the NUL) */

/* Capture nests: cap_begin/cap_end save+restore the enclosing capture on a small
 * stack, so a $(...) command substitution that runs another $(...) (e.g.
 * `$(echo $(echo x))`) keeps the outer capture intact. 16 levels is far beyond
 * the shell's cmdsub recursion cap, so the stack never fills in practice. */
#define CAP_STACK 16
static char         *g_capstk_buf[CAP_STACK];
static unsigned long  g_capstk_len[CAP_STACK], g_capstk_max[CAP_STACK];
static int            g_capsp = 0;

void cap_begin(void) {                 /* capture print() into a growable heap buffer */
    if (g_capsp < CAP_STACK) {         /* push the enclosing capture (if any) so it survives this one */
        g_capstk_buf[g_capsp] = g_capbuf;
        g_capstk_len[g_capsp] = g_caplen;
        g_capstk_max[g_capsp] = g_capmax;
        g_capsp++;
    }
    g_capmax = 65536;
    g_capbuf = malloc(g_capmax);
    g_caplen = 0;
    if (g_capbuf) g_capbuf[0] = '\0';
    else g_capmax = 0;                 /* malloc failed: capture off, print() falls through to the screen */
}
char *cap_end(unsigned long *outlen) { /* stop; hand the malloc'd buffer to the caller (which frees it) */
    char *b = g_capbuf;
    if (outlen) *outlen = g_caplen;
    if (g_capsp > 0) {                 /* pop: restore the enclosing capture */
        g_capsp--;
        g_capbuf = g_capstk_buf[g_capsp];
        g_caplen = g_capstk_len[g_capsp];
        g_capmax = g_capstk_max[g_capsp];
    } else { g_capbuf = 0; g_caplen = 0; g_capmax = 0; }
    return b;
}
int cap_active(void) { return g_capbuf != 0; }   /* is print() being captured (a pipe stage or $())? — so commands can suppress decorative output that would pollute the data */

void print(const char *s) {
    if (g_capbuf) {                    /* capture mode: append, growing the buffer as output accumulates */
        unsigned long i = 0;
        while (s[i]) {
            if (g_caplen + 1 >= g_capmax) {            /* full: double the buffer (cap at 32MB) */
                if (g_capmax >= (32u << 20)) break;    /* refuse to grow past 32MB (truncate gracefully) */
                unsigned long nc = g_capmax << 1;
                char *nb = realloc(g_capbuf, nc);
                if (!nb) break;                        /* OOM: keep what we captured so far */
                g_capbuf = nb; g_capmax = nc;
            }
            g_capbuf[g_caplen++] = s[i++];
        }
        g_capbuf[g_caplen] = '\0';
        return;
    }
    sys_write(1, s, ustrlen(s));
}

int readline(char *buf, int max) {
    if (max <= 0) return 0;            /* no room even for a terminator (and max-1 would underflow) */
    long n = sys_read(0, buf, (unsigned long)(max - 1));
    if (n < 0) n = 0;                  /* read rejected (e.g. the kernel refused the buffer): empty line, never buf[-1] */
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

/* ---- freestanding mem primitives -------------------------------------- *
 * Word-at-a-time so GCC's loop-pattern pass doesn't rewrite a naive byte loop
 * into a call to memcpy/memset (which would be infinite recursion), mirroring
 * the kernel's kernel/lib/string.c. GCC may emit calls to these from struct
 * copies / array inits even under -ffreestanding, so they must exist. */
typedef unsigned long uword_t;

void *memset(void *dst, int c, unsigned long n) {
    unsigned char *p = dst;
    unsigned char b = (unsigned char)c;
    if (n >= 8) {
        uword_t w = (uword_t)b;
        w |= w << 8; w |= w << 16; w |= w << 32;
        while (n && ((unsigned long)p & 7u)) { *p++ = b; n--; }
        while (n >= 8) { *(uword_t *)p = w; p += 8; n -= 8; }
    }
    while (n--) *p++ = b;
    return dst;
}
void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((((unsigned long)d ^ (unsigned long)s) & 7u) == 0) {
        while (n && ((unsigned long)d & 7u)) { *d++ = *s++; n--; }
        while (n >= 8) { *(uword_t *)d = *(const uword_t *)s; d += 8; s += 8; n -= 8; }
    }
    while (n--) *d++ = *s++;
    return dst;
}
void *memmove(void *dst, const void *src, unsigned long n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

/* malloc/free/calloc/realloc live in umalloc.c (host-testable in isolation). */

/* Program entry: the ELF entry point. Calls main() and exits with its result.
 * force_align_arg_pointer: the kernel enters _start with a 16-byte-aligned RSP
 * (the ELF entry-point convention), but GCC compiles _start as an ordinary
 * function assuming the post-CALL alignment (RSP ≡ 8 mod 16). That 8-byte skew
 * is invisible to integer code but makes SSE programs (DOOM, built with -msse2)
 * fault on the first aligned `movaps (%rsp)` in main. The attribute emits a
 * stack-realigning prologue so main and everything it calls get correct
 * 16-byte alignment regardless. Harmless for the non-SSE apps. */
extern int main(void);
__attribute__((force_align_arg_pointer))
void _start(void) {
    sys_exit(main());
    for (;;) { }
}
