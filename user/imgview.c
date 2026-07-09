/*
 * imgview.c — a graphical image viewer, a userspace program (M1392).
 *
 * Browses every image file on the disk and displays it fit-scaled, decoded by
 * the OS's OWN from-scratch decoders (PNG incl. interlaced, GIF, baseline JPEG,
 * BMP, SVG) via the new sys_loadimg syscall — the kernel reuses the same
 * decode_image() that backs the browser and the wallpaper. A caption shows the
 * filename, native pixel size and index. Keys: n/Right/Space = next, p/Left =
 * prev, q/Esc = quit. No FPU (all scaling is in the kernel).
 *
 * Launch: `run imgview` from the shell, or the Apps menu ("Image Viewer").
 */
#include "ulib.h"
#include "png.h"
#include "gif.h"
#include "jpeg.h"
#include "bmp.h"
#include "svg.h"
#include "webp.h"
#include <stddef.h>

#define W 480
#define H 384
#define IMGH 360                 /* image area; caption strip is the rest */

static unsigned *FB;
static unsigned char FONT[128 * 16];

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void ch(char c, int px, int py, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col); }
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }

static int ci(char a) { return (a >= 'A' && a <= 'Z') ? a + 32 : a; }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int ext_is(const char *n, const char *e) {            /* does n end with ".<e>" (case-insensitive)? */
    int ln = slen(n), le = slen(e);
    if (ln < le + 1 || n[ln - le - 1] != '.') return 0;
    for (int i = 0; i < le; i++) if (ci(n[ln - le + i]) != ci(e[i])) return 0;
    return 1;
}
static int is_img(const char *n) { return ext_is(n,"png")||ext_is(n,"gif")||ext_is(n,"jpg")||ext_is(n,"jpeg")||ext_is(n,"bmp")||ext_is(n,"svg")||ext_is(n,"webp")||ext_is(n,"web"); }
static int eq_ci(const char *a, const char *b) { int i = 0; while (a[i] && b[i]) { if (ci(a[i]) != ci(b[i])) return 0; i++; } return a[i] == b[i]; }
static void putint(char *b, int *p, int v) { if (v < 0) { b[(*p)++] = '-'; v = -v; }
    char t[10]; int i = 0; if (v == 0) t[i++] = '0'; while (v) { t[i++] = '0' + v % 10; v /= 10; } while (i) b[(*p)++] = t[--i]; }

/* RING-3 image decode + fit-scale: the from-scratch decoders run in THIS process,
 * not the kernel, so a malformed image opened in the viewer crashes only imgview,
 * not the kernel. (Replaces the in-kernel sys_loadimg decode path.) The decoders
 * need a few libc helpers ulib doesn't provide (it has memset/memcpy/memmove). */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i]; return 0; }

#define DCAP (4 * 1024 * 1024)                  /* >= 1M pixels * 4 (decode_image's cap) */
#define SCRCAP (24 * 1024 * 1024)                /* WebP's VP8L decoder needs up to ~20 MB scratch (webp.c); BSS is lazily faulted */
static unsigned char g_file[512 * 1024], g_dec[DCAP], g_scr[SCRCAP];

static int decode_native(const char *name, int *nw, int *nh) {
    long n = sys_readfile(name, g_file, sizeof g_file);
    if (n <= 0) return -1;
    unsigned char *d = g_file; int len = (int)n, w = 0, h = 0, rc = -1;
    if      (len >= 8 && d[0] == 0x89 && d[1] == 'P') rc = png_decode(d, len, g_dec, DCAP, g_scr, DCAP, &w, &h);
    else if (len >= 4 && d[0] == 'G'  && d[1] == 'I') rc = gif_decode(d, len, g_dec, DCAP, g_scr, DCAP, &w, &h);
    else if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8) rc = jpeg_decode(d, len, g_dec, DCAP, g_scr, DCAP, &w, &h);
    else if (len >= 2 && d[0] == 'B'  && d[1] == 'M') rc = bmp_decode(d, len, g_dec, DCAP, &w, &h);
    else if (len >= 1 && d[0] == '<') rc = svg_decode(d, len, g_dec, DCAP, g_scr, DCAP, &w, &h);
    else if (len >= 12 && d[0]=='R'&&d[1]=='I'&&d[2]=='F'&&d[3]=='F'&&d[8]=='W'&&d[9]=='E'&&d[10]=='B'&&d[11]=='P')
        rc = webp_decode(d, len, g_dec, DCAP, g_scr, SCRCAP, &w, &h);   /* lossless (VP8L) WebP — needs the big scratch */
    if (rc != 0 || w <= 0 || h <= 0) return -1;
    *nw = w; *nh = h; return 0;
}

/* nearest-neighbour fit-scale the decoded RGBA (nw x nh) into the W x IMGH image
 * area, aspect-preserving + centred (letterboxed). */
static void blit_scaled(int nw, int nh) {
    for (int i = 0; i < IMGH * W; i++) FB[i] = 0x14181E;
    int dw = W, dh = (int)((long)nh * W / nw);
    if (dh > IMGH) { dh = IMGH; dw = (int)((long)nw * IMGH / nh); }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = (W - dw) / 2, oy = (IMGH - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * nh / dh); if (sy >= nh) sy = nh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long)x * nw / dw); if (sx >= nw) sx = nw - 1;
            unsigned char *p = &g_dec[((long)sy * nw + sx) * 4];
            FB[(oy + y) * W + (ox + x)] = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
        }
    }
}

static char names[64][24];
static int nimg;

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("imgview: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("imgview: init failed\n"); return 1; }

    char *lst = (char *)malloc(16384);
    if (lst) { if (sys_list(lst, 16384) < 0) lst[0] = 0; }
    for (int i = 0; lst && lst[i] && nimg < 64; ) {           /* parse "name size...\n" lines, keep image files */
        char nm[24]; int p = 0;
        while (lst[i] && lst[i] != ' ' && lst[i] != '\n' && p < 23) nm[p++] = lst[i++];
        nm[p] = 0;
        while (lst[i] && lst[i] != '\n') i++;
        if (lst[i] == '\n') i++;
        if (p > 0 && is_img(nm)) { for (int k = 0; k <= p && k < 24; k++) names[nimg][k] = nm[k]; nimg++; }
    }

    int idx = 0, prev = -1, outwh[2] = { 0, 0 };
    char arg[24];                                            /* `run imgview NAME` (or the file manager) opens that image */
    if (sys_getarg(arg, sizeof arg) > 0)
        for (int i = 0; i < nimg; i++) if (eq_ci(names[i], arg)) { idx = i; break; }
    int prevb = 0;
    for (;;) {
        if (idx != prev) {
            prev = idx;
            if (nimg == 0) {
                for (int i = 0; i < W * H; i++) FB[i] = 0x14181E;
                text("No image files on disk.", 20, 32, 0xE0A0A0);
                text("(expected TEST.PNG, PHOTO.JPG, LOGO.GIF, ...)", 20, 56, 0x808890);
            } else {
                int nw = 0, nh = 0;
                if (decode_native(names[idx], &nw, &nh) == 0) { blit_scaled(nw, nh); outwh[0] = nw; outwh[1] = nh; }
                else { for (int i = 0; i < IMGH * W; i++) FB[i] = 0x281418; text("decode failed", 20, 32, 0xF08080); outwh[0] = outwh[1] = 0; }
                fill(0, IMGH, W, H - IMGH, 0x101418);
                fill(0, IMGH, W, 1, 0x2A3340);                /* separator above the caption */
                char cap[64]; int p = 0;
                for (int k = 0; names[idx][k] && p < 20; k++) cap[p++] = names[idx][k];
                cap[p++] = ' '; cap[p++] = ' ';
                putint(cap, &p, outwh[0]); cap[p++] = 'x'; putint(cap, &p, outwh[1]);
                cap[p++] = ' '; cap[p++] = '['; putint(cap, &p, idx + 1); cap[p++] = '/'; putint(cap, &p, nimg); cap[p++] = ']';
                cap[p] = 0;
                text(cap, 8, IMGH + 5, 0xC8D0DE);
                text("n/p  w:wall  q:quit", W - 19 * 8 - 6, IMGH + 5, 0x707888);
            }
            sys_gfx_blit(FB);
        }
        int k = sys_pollkey();
        if (k == 'q' || k == 27) break;
        else if (nimg > 0 && (k == 'n' || k == ' ' || k == 0x14)) idx = (idx + 1) % nimg;
        else if (nimg > 0 && (k == 'p' || k == 0x13)) idx = (idx + nimg - 1) % nimg;
        else if (k == 'w' && nimg > 0) sys_setwall(names[idx]);   /* set the current image as the desktop wallpaper (M1422) */
        int mx, my, b = sys_mouse(&mx, &my);                  /* click left half = prev, right half = next (M1397) */
        if ((b & 1) && !(prevb & 1) && mx >= 0 && nimg > 0) idx = (mx < W / 2) ? (idx + nimg - 1) % nimg : (idx + 1) % nimg;
        prevb = b;
        sys_sleep(70);
    }
    return 0;
}
