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

/* --- network: route to the WORKING kernel TLS/HTTP syscalls (no crypto in ring 3) --- */
int http_get(const char *host, const char *path, char *out, int max) { return (int)sys_http(host, path, out, (unsigned long)max); }
int tls_get(const char *host, const char *path, uint8_t *out, int max, uint32_t seed) { (void)seed; return (int)sys_https(host, path, out, (unsigned long)max); }
/* browser.c's EventSource/streaming path calls the *_sse fetch variants; in ring 3
 * the kernel syscall does the (non-streaming) fetch, so route them the same way. */
int http_get_sse(const char *host, const char *path, char *out, int max) { return (int)sys_http(host, path, out, (unsigned long)max); }
int tls_get_sse(const char *host, const char *path, uint8_t *out, int max, uint32_t seed) { (void)seed; return (int)sys_https(host, path, out, (unsigned long)max); }
int http_post(const char *h, const char *p, const char *ct, const char *b, int bl, char *o, int max) { (void)h; (void)p; (void)ct; (void)b; (void)bl; (void)o; (void)max; return -1; }
int tls_post(const char *h, const char *p, const char *ct, const char *b, int bl, uint8_t *o, int max, uint32_t s) { (void)h; (void)p; (void)ct; (void)b; (void)bl; (void)o; (void)max; (void)s; return -1; }
/* cert info: the kernel already validated the chain in sys_https; report secure. */
int tls_cert_status(void) { return 1; }
int tls_chain_anchored(void) { return 1; }
int tls_host_match(void) { return 1; }
const char *tls_leaf_cn(void) { return ""; }
const char *tls_leaf_expiry(void) { return ""; }

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
