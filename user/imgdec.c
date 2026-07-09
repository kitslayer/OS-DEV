/*
 * imgdec.c — the from-scratch image decoders, running in RING 3.
 *
 * The second step (after jsrun) of moving the browser's parser stack out of the
 * kernel. PNG/GIF/JPEG/SVG/BMP decoders are a classic kernel-exploit surface
 * (malformed images are a favourite fuzz target), and they ran IN THE KERNEL.
 * They are pure compute over caller-provided buffers (already host-fuzzed in
 * tests/), so the Makefile links kernel/{png,gif,jpeg,bmp,svg,inflate}.c straight
 * into this ring-3 program. A bug in a decoder now crashes only this process.
 *
 * It reads each image it finds on the disk, decodes it by magic bytes, and writes
 * "<file>: <FMT> <w>x<h> checksum=<n>" lines to IMGDEC.TXT (inspect with
 * `cat IMGDEC.TXT` from the shell). The in-kernel decode_image() path is untouched.
 */
#include "ulib.h"
#include "png.h"
#include "gif.h"
#include "jpeg.h"
#include "bmp.h"
#include "svg.h"
#include "webp.h"
#include <stddef.h>

/* libc helpers the decoders/inflate use that user/ulib.c doesn't provide. */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i]; return 0; }

#define OUTCAP  (4 * 1024 * 1024)   /* >= 1M pixels * 4 (decode_image's pixel cap) */
#define SCRCAP  (24 * 1024 * 1024)  /* WebP's VP8L decoder needs up to ~20 MB scratch (webp.c); BSS is lazily faulted */
static uint8_t g_file[512 * 1024];
static uint8_t g_out[OUTCAP];
static uint8_t g_scratch[SCRCAP];
static char    g_report[8192];
static int     g_rlen;

static void rep(const char *s) { while (*s && g_rlen < (int)sizeof(g_report) - 1) g_report[g_rlen++] = *s++; g_report[g_rlen] = 0; }
static void rep_int(long v) { char b[24]; int i = 0; if (v < 0) { rep("-"); v = -v; } if (!v) b[i++] = '0'; while (v) { b[i++] = (char)('0' + v % 10); v /= 10; } while (i) { char c[2] = { b[--i], 0 }; rep(c); } }

static void decode_one(const char *name) {
    long n = sys_readfile(name, g_file, sizeof(g_file));
    if (n <= 0) return;                       /* not present / unreadable — skip */
    const uint8_t *d = g_file; int len = (int)n, w = 0, h = 0, rc = -1;
    const char *fmt = "unknown";
    if (len >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        fmt = "PNG";  rc = png_decode(d, len, g_out, OUTCAP, g_scratch, SCRCAP, &w, &h);
    } else if (len >= 4 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') {
        fmt = "GIF";  rc = gif_decode(d, len, g_out, OUTCAP, g_scratch, SCRCAP, &w, &h);
    } else if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        fmt = "JPEG"; rc = jpeg_decode(d, len, g_out, OUTCAP, g_scratch, SCRCAP, &w, &h);
    } else if (len >= 2 && d[0] == 'B' && d[1] == 'M') {
        fmt = "BMP";  rc = bmp_decode(d, len, g_out, OUTCAP, &w, &h);
    } else if (len >= 1 && d[0] == '<') {
        fmt = "SVG";  rc = svg_decode(d, len, g_out, OUTCAP, g_scratch, SCRCAP, &w, &h);
    } else if (len >= 12 && d[0]=='R'&&d[1]=='I'&&d[2]=='F'&&d[3]=='F'&&d[8]=='W'&&d[9]=='E'&&d[10]=='B'&&d[11]=='P') {
        fmt = "WEBP"; rc = webp_decode(d, len, g_out, OUTCAP, g_scratch, SCRCAP, &w, &h);
    }
    rep(name); rep(": ");
    if (rc == 0 && w > 0 && h > 0) {
        unsigned long sum = 0; long px = (long)w * h * 4;
        if (px > OUTCAP) px = OUTCAP;
        for (long i = 0; i < px; i++) sum = sum * 31u + g_out[i];
        rep(fmt); rep(" "); rep_int(w); rep("x"); rep_int(h); rep(" checksum="); rep_int((long)(sum & 0x7fffffff));
    } else {
        rep(fmt); rep(" decode FAILED (rc="); rep_int(rc); rep(")");
    }
    rep("\n");
}

int main(void) {
    /* Defense-in-depth (pledge): the decoders only read image files (rpath),
     * write the report (wpath), and use basic stdio. A malformed-image bug in a
     * decoder can't spawn, exec, touch the network, or the GPU. "thread" (M1533)
     * lets jpeg.c's color-conversion pass spawn worker threads sharing THIS
     * address space (sys_clone) — narrower than PL_PROC, so it still can't spawn
     * a new process or exec. */
    sys_pledge("stdio rpath wpath thread");
    rep("from-scratch image decoders, now running in RING 3 (not the kernel):\n\n");
    /* Try every image the demo disk ships; decode whichever are present. */
    static const char *files[] = { "TEST.PNG", "ICON.PNG", "PHOTO.JPG", "ANIM.GIF",
                                    "LOGO.SVG", "ICON.SVG", "GRAD.SVG", 0 };
    for (int i = 0; files[i]; i++) decode_one(files[i]);
    print(g_report);
    sys_writefile("IMGDEC.TXT", g_report, (unsigned long)g_rlen);
    return 0;
}
