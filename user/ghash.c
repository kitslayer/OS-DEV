/*
 * ghash.c — a file hash / checksum tool, a userspace program (M1708).
 *
 * Point it at a file and it shows the from-scratch SHA-256 and SHA-512 (computed
 * in the kernel via sys_sha256/512), the CRC-32, the size, and the Base64 of the
 * contents — handy for verifying a download's integrity. CRC-32 and Base64 (not
 * syscalls) live in the pure user/hashcore.h, host-unit-tested by tests/hash;
 * this file is the UI + file read.
 *
 * Launch: `ghash FILE` (or `ghash` for a built-in demo, README.TXT). Esc/q quit.
 */
#include "ulib.h"
#include "hashcore.h"       /* hc_crc32 / hc_b64_encode / hc_hex32 (host-tested by tests/hash) */

#define IOMAX (2 * 1024 * 1024)

static unsigned char buf[IOMAX];
static char fname[64];
static char sha256[80], sha512[160], b64[8192];
static unsigned crc;
static long fsize;

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }
static void putn(long v) { char b[16]; int n = 0; if (v == 0) b[n++] = '0'; while (v) { b[n++] = (char)('0' + v % 10); v /= 10; } char c[2] = {0,0}; while (n) { c[0] = b[--n]; print(c); } }

static void compute(void) {
    long n = sys_readfile(fname, buf, IOMAX);
    fsize = n;
    if (n < 0) return;
    crc = hc_crc32(buf, n);
    hc_b64_encode(buf, n, b64, sizeof b64);
    if (sys_sha256(fname, sha256, sizeof sha256) < 0) scopy(sha256, "(unavailable)", sizeof sha256);
    if (sys_sha512(fname, sha512, sizeof sha512) < 0) scopy(sha512, "(unavailable)", sizeof sha512);
}

/* print `s` wrapped to `w` cols per line, each continuation line indented `ind` */
static void print_wrapped(const char *s, int w, int ind) {
    int col = ind;
    char c[2] = {0,0};
    for (int i = 0; s[i]; i++) {
        if (col >= w) { print("\n"); for (int k = 0; k < ind; k++) print(" "); col = ind; }
        c[0] = s[i]; print(c); col++;
    }
    print("\n");
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print(" hash "); sys_setcolor(1); print(fname);
    if (fsize < 0) { sys_setcolor(2); print("   cannot read file\n"); sys_setcolor(0); return; }
    sys_setcolor(8); print("   "); putn(fsize); print(" bytes\n\n");

    sys_setcolor(6); print(" CRC-32   "); sys_setcolor(3);
    { char h[9]; hc_hex32(crc, h); print(h); } print("\n\n");
    sys_setcolor(6); print(" SHA-256  "); sys_setcolor(10); print(sha256); print("\n\n");
    sys_setcolor(6); print(" SHA-512  "); sys_setcolor(10); print_wrapped(sha512, 74, 10); print("\n");
    sys_setcolor(6); print(" base64   "); sys_setcolor(1); print_wrapped(b64, 74, 10);

    sys_setcolor(8); print("\n Esc quit");
    sys_setcolor(0);
}

int main(void) {
    char arg[64];
    if (sys_getarg(arg, sizeof arg) > 0 && arg[0] && !streq(arg, "demo")) scopy(fname, arg, sizeof fname);
    else scopy(fname, "README.TXT", sizeof fname);
    compute();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 27 || k == 'q') break;
    }
    sys_clear();
    return 0;
}
