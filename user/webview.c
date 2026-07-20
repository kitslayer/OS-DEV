/*
 * webview.c — the from-scratch web browser, running in RING 3.
 *
 * The headline ring-0 -> ring-3 migration: the mature browser engine (kernel/
 * browser.c, ~4500 lines: HTML tokenizer, CSS cascade, layout, the JS bridge,
 * image decode) is the OS's biggest untrusted-input attack surface and ran in the
 * kernel. This builds it (and js.c + the image decoders + the HTML/CSS/URL parsers)
 * as an ordinary ring-3 program with -DBROWSER_RING3, so a bug anywhere in that
 * parsing/rendering pipeline crashes only this process, not the kernel.
 *
 * Network/TLS stay in the kernel for now (fetch via the working SYS_http/SYS_https
 * syscalls — the crypto already runs there and is validated), so only the
 * *parsing* moves out. The kernel browser keeps working until this replaces it.
 *
 * This file is the ring-3 host: it shims browser.c's kernel calls (fb_* -> a
 * pixel canvas, kmalloc -> malloc, http_get/tls_get -> syscalls, vfs -> file
 * syscalls, timer -> uptime) and runs the event loop (render -> blit; keys/mouse
 * -> browser_key/click).
 */
#include "ulib.h"
#include "browser.h"
#include "net.h"     /* tcp_conn + the tcp_* prototypes tls.c's TCP shims implement (M1863) */
#include "rtc.h"
#include <stddef.h>
#include <stdint.h>

#define W 960
#define H 720
static uint32_t *CANVAS;
static unsigned char FONT[128 * 16];

/* --- libc helpers browser.c/js.c/decoders/parsers need (ulib provides memset/memcpy/memmove) --- */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i]; return 0; }

/* --- memory: kernel heap -> userspace malloc --- */
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { void *p = malloc(n); if (p) memset(p, 0, n); return p; }
void  kfree(void *p) { free(p); }

/* --- kprintf: discard the browser's debug output --- */
void kprintf(const char *f, ...) { (void)f; }

/* --- framebuffer -> the gfx canvas --- */
int fb_width(void) { return W; }
int fb_height(void) { return H; }
void fb_pixel(int x, int y, uint32_t c) { if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) CANVAS[y * W + x] = c; }
void fb_fill_rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x; if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) CANVAS[(y + j) * W + (x + i)] = c;
}
void fb_row(int x, int y, int w, const uint32_t *colors) {   /* browser.c's <img> compositing (M1513) */
    if (y < 0 || y >= H) return;
    int lo = x < 0 ? -x : 0, hi = w;
    if (x + hi > W) hi = W - x;
    for (int i = lo; i < hi; i++) CANVAS[y * W + x + i] = colors[i];
}
static void g_draw(int x, int y, char ch, uint32_t fg, int hasbg, uint32_t bg, int sc) {
    unsigned u = (unsigned char)ch; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16];
    for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) {
        int on = (g[r] >> (7 - b)) & 1;
        if (on || hasbg) {
            uint32_t col = on ? fg : bg;
            for (int yy = 0; yy < sc; yy++) for (int xx = 0; xx < sc; xx++) fb_pixel(x + b * sc + xx, y + r * sc + yy, col);
        }
    }
}
void fb_glyph(int x, int y, char c, uint32_t fg, uint32_t bg) { g_draw(x, y, c, fg, 1, bg, 1); }
void fb_glyph_fg(int x, int y, char c, uint32_t fg) { g_draw(x, y, c, fg, 0, 0, 1); }
void fb_text(int x, int y, const char *s, uint32_t c, int sc) { if (sc < 1) sc = 1; for (int i = 0; s[i]; i++) { g_draw(x, y, s[i], c, 0, 0, sc); x += 8 * sc; } }

/* --- network -------------------------------------------------------------
 * HTTPS fetch now runs ENTIRELY in ring 3 (M1863). tls.c + the crypto/X.509
 * stack are linked into this program (see the Makefile webview rule), so
 * browser.c's tls_get/tls_get_sse/tls_post and the tls_* cert-info accessors
 * are the REAL functions from tls.c — the kernel's SYS_https (and with it the
 * whole TLS/crypto/cert-validation path in ring 0) is no longer on the
 * browser's fetch path. tls.c reaches the wire through the tcp/DNS shims
 * below onto ring-3 syscalls, exactly like the standalone httpget program.
 * A single fetch worker (browser.c's g_worker, claimed atomically) does every
 * fetch sequentially, so tls.c's shared static handshake buffers never see two
 * callers at once (its TLS_RING3 build assumes exactly this single-caller model).
 *
 * Plain http:// still uses SYS_http: it carries no crypto/X.509, so it isn't
 * the ring-0 attack surface this migration closes — only the TLS path moved. */
int http_get(const char *host, const char *path, char *out, int max) { return (int)sys_http(host, path, out, (unsigned long)max); }
int http_get_sse(const char *host, const char *path, char *out, int max) { return (int)sys_http(host, path, out, (unsigned long)max); }
int http_post(const char *h, const char *p, const char *ct, const char *b, int bl, char *o, int max) { (void)h; (void)p; (void)ct; (void)b; (void)bl; (void)o; (void)max; return -1; }
/* WebSocket (M1846): the kernel holds the connection between the two calls (one
 * socket at a time), so these route straight through to the kernel transport.
 * (wss:// still uses the kernel TLS via sys_ws_open — a separate path from the
 * page-fetch tls_get above; moving it is out of scope for this migration.) */
int ws_open(const char *url, int *status) { return (int)sys_ws_open(url, status); }
int ws_exchange(int id, const char *sendbuf, int sendtot, char *out, int outmax, int *nrecv) { return (int)sys_ws_exchange(id, sendbuf, sendtot, out, outmax, nrecv); }

/* tls.c's kernel TCP/DNS/clock hooks, shimmed onto ring-3 syscalls — identical
 * to user/httpget.c (the standalone ring-3 TLS client this shares its stack with). */
static int g_sock = -1;
int tcp_connect(tcp_conn *c, const uint8_t ip[4], uint16_t port) {
    (void)c; g_sock = sys_socket(2, 1); if (g_sock < 0) return -1;   /* AF_INET, SOCK_STREAM */
    return sys_connect(g_sock, ip, (int)port);                       /* 0 / -1 */
}
int tcp_write(tcp_conn *c, const uint8_t *data, int len) { (void)c; return (int)sys_fdwrite(g_sock, data, (unsigned long)len); }
int tcp_read(tcp_conn *c, uint8_t *out, int max, uint64_t ticks) { (void)c; (void)ticks; return (int)sys_fdread(g_sock, out, (unsigned long)max); }
void tcp_close(tcp_conn *c) { (void)c; if (g_sock >= 0) { sys_fdclose(g_sock); g_sock = -1; } }
/* dotted-quad literal -> 4 bytes (tls.c calls this before dns_resolve, M1847). */
int parse_ipv4(const char *s, uint8_t out[4]) {
    int oct = 0, v = 0, any = 0;
    for (int i = 0; i < 4; i++) out[i] = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); any = 1; if (v > 255) return -1; }
        else if (*p == '.' || *p == 0) {
            if (!any || oct > 3) return -1;
            out[oct++] = (uint8_t)v; v = 0; any = 0;
            if (*p == 0) break;
        } else return -1;
    }
    return oct == 4 ? 0 : -1;
}
int dns_resolve(const char *host, uint8_t out_ip[4]) {
    char b[80];
    long r = sys_resolve(host, b, sizeof b);      /* 0 on success, -1 on failure; IP as "a.b.c.d\n\0" */
    if (r < 0) return -1;
    b[sizeof b - 1] = 0;
    int oi = 0, v = 0, have = 0;
    for (int i = 0; i < (int)sizeof b && b[i] && oi < 4; i++) {
        char ch = b[i];
        if (ch >= '0' && ch <= '9') { v = v * 10 + (ch - '0'); have = 1; }
        else if (have) { out_ip[oi++] = (uint8_t)v; v = 0; have = 0; }
    }
    if (have && oi < 4) out_ip[oi++] = (uint8_t)v;
    return oi == 4 ? 0 : -1;
}
/* ecdsa.c (X.509 verify) reads smp_current_cpu() to index its per-core scratch
 * slot. A constant 0 pins every ECDSA op to slot 0 — correct AND migration-safe
 * here: the single fetch worker never runs two verifies at once, and a constant
 * (vs a value that could change mid-call under M1531 core migration) is exactly
 * what avoids the per-core-slot hazard. */
int smp_current_cpu(void) { return 0; }

/* --- vfs (bookmarks/history/SITES) -> file syscalls --- */
long vfs_read(const char *name, void *buf, unsigned long max) { return sys_readfile(name, buf, max); }
long vfs_write(const char *name, const void *buf, unsigned long len) { return sys_writefile(name, buf, len); }

/* --- timing --- */
uint64_t timer_ticks(void) { return sys_uptime_ms() / 10; }   /* ~100 Hz ticks */
void timer_wait(int t) { sys_sleep(t * 10); }

/* js.c's Date object calls rtc_now() — give it the real wall clock via sys_time. */
void rtc_now(struct rtc_time *t) {
    t->year = 2026; t->month = 1; t->day = 1; t->hour = 0; t->min = 0; t->sec = 0;
    char b[48]; long n = sys_time(b, sizeof b); if (n <= 0) return;   /* "YYYY-MM-DD HH:MM:SS" */
    int *f[6] = { &t->year, &t->month, &t->day, &t->hour, &t->min, &t->sec };
    int fi = 0, v = 0, have = 0;
    for (long i = 0; i < n && fi < 6; i++) { char c = b[i];
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); have = 1; }
        else if (have) { *f[fi++] = v; v = 0; have = 0; } }
    if (have && fi < 6) *f[fi++] = v;
}

/* task_create_stack -> a real ring-3 thread (sys_clone) for browser.c's async
 * fetch worker, so the UI keeps rendering ("Loading...") while a page downloads.
 * The worker thread shares this address space, so browser.c's g_req/need_parse
 * coordination flags work across the two threads. */
typedef struct task task_t;
task_t *task_create_stack(void (*entry)(void), uint64_t cr3, void *proc, int sz) {
    (void)cr3; (void)proc;
    if (sz <= 0) sz = 256 * 1024;
    char *stk = (char *)malloc((unsigned long)sz);
    if (stk) sys_clone((void *)entry, stk + sz, 0);   /* new thread runs entry() */
    return (task_t *)1;
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("webview: gfx init failed\n"); return 1; }
    CANVAS = (uint32_t *)malloc((unsigned long)W * H * 4);
    if (!CANVAS || sys_font(FONT, sizeof FONT) < 0) { print("webview: init failed\n"); return 1; }

    browser_t *b = browser_create(0);            /* built-in start page (no network) */
    if (!b) { print("webview: create failed\n"); return 1; }

    /* Defense-in-depth (pledge, M1074): the fetch worker thread is already spawned
     * (browser_create -> sys_clone), so we can now drop PL_PROC/EXEC/VM/POWER. A
     * parser bug that gains code execution inside this ring-3 browser can no longer
     * spawn, exec, fork, kill, mmap, or power off — only gfx + network + file I/O,
     * the classes the browser legitimately needs. (pledge kills on a violation.)
     * "thread" (PL_THREAD, M1533): jpeg.c's color-conversion pass spawns worker
     * threads on every page/image load (sys_clone), NOT just once at startup like
     * the fetch worker above — narrower than PL_PROC, so this still can't spawn a
     * new process/exec, only more threads sharing this same sandboxed address space. */
    sys_pledge("stdio rpath wpath inet gfx thread");

    int pb = 0;
    for (;;) {
        browser_poll(b);                          /* process any pending fetch/parse */
        browser_render(b, 0, 0, W, H);
        sys_gfx_blit(CANVAS);
        int k = sys_pollkey();
        if (k) browser_key(b, k);
        int mx, my, btn = sys_mouse(&mx, &my);
        if ((btn & 1) && !(pb & 1) && mx >= 0) browser_click(b, mx, my, W, H);
        pb = btn;
        sys_sleep(25);
    }
    return 0;
}
