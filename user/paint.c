/*
 * paint.c — an ASCII-art canvas, a userspace program.
 *
 * Move a cursor with the arrows; any printable key stamps that glyph at the
 * cursor (and steps right, wrapping), in the current brush colour. Tab cycles
 * the colour, Backspace rubs out, and ESC saves the picture to PAINT.TXT and
 * quits. A small creative tool that shows off the per-cell colour palette (M311).
 */
#include "ulib.h"

#define W 42
#define H 14

static char         canv[H][W];
static unsigned char canc[H][W];
static int cx, cy;
static int brush = 2;          /* palette colour 1..15 (start red) */

static void save(void) {
    char buf[H * (W + 1) + 1]; int p = 0;
    for (int y = 0; y < H; y++) {
        int end = W; while (end > 0 && canv[y][end - 1] == ' ') end--;   /* trim trailing blanks */
        for (int x = 0; x < end; x++) buf[p++] = canv[y][x];
        buf[p++] = '\n';
    }
    sys_writefile("PAINT.TXT", buf, (unsigned long)p);
}

static void render(void) {
    sys_clear();
    /* status line */
    sys_setcolor(8); print(" paint  ");
    char nb[8]; int q = 0; nb[q++] = (char)('0' + (cy + 1) / 10 % 10); nb[q++] = (char)('0' + (cy + 1) % 10);
    nb[q++] = ','; nb[q++] = (char)('0' + (cx + 1) / 10 % 10); nb[q++] = (char)('0' + (cx + 1) % 10); nb[q] = 0;
    print(nb);
    print("  brush "); sys_setcolor(brush); print("##");
    sys_setcolor(8); print("  Tab=col BS=del ESC=save&quit\n");

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            char ch = canv[y][x];
            if (x == cx && y == cy) {                 /* cursor cell, highlighted white */
                sys_setcolor(1);
                char cb[2] = { ch == ' ' ? '_' : ch, 0 }; print(cb);
            } else {
                sys_setcolor(ch == ' ' ? 0 : canc[y][x]);
                char cb[2] = { ch, 0 }; print(cb);
            }
        }
        print("\n");
    }
    sys_setcolor(0);
}

int main(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { canv[y][x] = ' '; canc[y][x] = 0; }
    /* load an existing picture so it can be re-edited (colour isn't stored, so it loads green) */
    char fb[H * (W + 2) + 4];
    long n = sys_readfile("PAINT.TXT", fb, sizeof(fb) - 1);
    if (n > 0) {
        int x = 0, y = 0;
        for (long i = 0; i < n && y < H; i++) {
            char c = fb[i];
            if (c == '\n') { x = 0; y++; }
            else { if (x < W && c != '\r') canv[y][x] = c; x++; }
        }
    }
    cx = cy = 0;
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }
        if (k == 27) { save(); return 0; }                          /* ESC: save + quit */
        else if (k == 9) brush = brush % 15 + 1;                    /* Tab: next colour */
        else if (k == 0x11) { if (cy > 0) cy--; }                   /* arrows: move */
        else if (k == 0x12) { if (cy < H - 1) cy++; }
        else if (k == 0x13) { if (cx > 0) cx--; }
        else if (k == 0x14) { if (cx < W - 1) cx++; }
        else if (k == 8 || k == 127) {                             /* Backspace: step back + erase */
            if (cx > 0) cx--; else if (cy > 0) { cy--; cx = W - 1; }
            canv[cy][cx] = ' '; canc[cy][cx] = 0;
        }
        else if (k >= 32 && k <= 126) {                            /* printable: stamp + advance */
            canv[cy][cx] = (char)k; canc[cy][cx] = (unsigned char)brush;
            if (++cx >= W) { cx = 0; if (cy < H - 1) cy++; }
        }
        else continue;
        render();
    }
}
