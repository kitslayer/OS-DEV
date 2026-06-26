/*
 * gtodo.c — a to-do list, a userspace program (M1418).
 *
 * A persistent checklist: a = add an item (type, Enter to commit, Esc cancels),
 * Up/Down select, Space toggles done, d deletes, q/Esc quits. The list is saved
 * to TODO.TXT on every change (one item per line: "1 text" done, "0 text" not)
 * and reloaded on launch, so it survives across runs.
 *
 * Launch: `run gtodo` from the shell, or the Apps menu ("To-Do").
 */
#include "ulib.h"

#define W 460
#define H 320
#define MAXIT 24
#define TLEN 52

static unsigned *FB;
static unsigned char FONT[128 * 16];
static struct { char t[TLEN]; char done; } items[MAXIT];
static int nit = 0;
static const char *FNAME = "TODO.TXT";

static void putpx(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) FB[y * W + x] = c; }
static void fill(int x0, int y0, int w, int h, unsigned c) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) putpx(x, y, c); }
static void ch(char c, int px, int py, unsigned col) { unsigned u = (unsigned char)c; if (u >= 128) u = '?';
    const unsigned char *g = &FONT[u * 16]; for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if ((g[r] >> (7 - b)) & 1) putpx(px + b, py + r, col); }
static void text(const char *s, int x, int y, unsigned col) { for (int i = 0; s[i]; i++) { ch(s[i], x, y, col); x += 8; } }
static int putdec(char *b, int p, int v) {
    char t[8]; int ti = 0;
    if (v == 0) t[ti++] = '0';
    while (v) { t[ti++] = '0' + v % 10; v /= 10; }
    while (ti) b[p++] = t[--ti];
    return p;
}

static void load(void) {
    static char buf[4096];
    long n = sys_readfile(FNAME, buf, sizeof buf - 1);
    nit = 0;
    if (n <= 0) return;
    buf[n] = 0;
    for (int i = 0; buf[i] && nit < MAXIT; ) {
        int eol = i; while (buf[eol] && buf[eol] != '\n') eol++;
        if (eol >= i + 2) {                                /* "D text" -> done flag + text */
            items[nit].done = (buf[i] == '1');
            int p = 0; for (int j = i + 2; j < eol && p < TLEN - 1; j++) items[nit].t[p++] = buf[j];
            items[nit].t[p] = 0; nit++;
        }
        i = (buf[eol] == '\n') ? eol + 1 : eol;
    }
}
static void save(void) {
    static char buf[4096]; int p = 0;
    for (int k = 0; k < nit && p < 4000; k++) {
        buf[p++] = items[k].done ? '1' : '0'; buf[p++] = ' ';
        for (int j = 0; items[k].t[j] && p < 4090; j++) buf[p++] = items[k].t[j];
        buf[p++] = '\n';
    }
    sys_writefile(FNAME, buf, p);
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gtodo: gfx init failed\n"); return 1; }
    FB = (unsigned *)malloc((unsigned long)W * H * 4);
    if (!FB || sys_font(FONT, sizeof FONT) < 0) { print("gtodo: init failed\n"); return 1; }
    load();

    int sel = 0, editing = 0, elen = 0, dirty = 1;
    char eb[TLEN] = { 0 };
    for (;;) {
        if (dirty) {
            for (int i = 0; i < W * H; i++) FB[i] = 0x141820;
            text("To-Do List", 14, 10, 0x8FD0FF);
            { char c[16]; int p = 0, done = 0;
              for (int k = 0; k < nit; k++) if (items[k].done) done++;
              p = putdec(c, p, done); c[p++] = '/'; p = putdec(c, p, nit); c[p] = 0;
              text(c, W - p * 8 - 14, 10, 0x808A9A); }
            for (int x = 8; x < W - 8; x++) putpx(x, 30, 0x2A3040);

            if (nit == 0 && !editing) text("(empty - press a to add an item)", 18, 44, 0x707888);
            for (int k = 0; k < nit; k++) {
                int y = 40 + k * 22;
                if (k == sel && !editing) fill(8, y - 2, W - 16, 20, 0x243050);
                text(items[k].done ? "[x]" : "[ ]", 16, y, items[k].done ? 0x70E090 : 0xD0B050);
                text(items[k].t, 52, y, items[k].done ? 0x707888 : 0xE0E8F4);
            }
            if (editing) {                                  /* the new-item input line */
                int y = 40 + nit * 22 + 6;
                text(editing == 2 ? "Edit:" : "New:", 16, y, 0x8FD0FF);
                text(eb, 56, y, 0xF0F0A0);
                fill(56 + elen * 8, y, 8, 16, 0x808890);    /* caret */
            }
            text(editing ? "Enter: save   Esc: cancel" : "a: add   e: edit   space: done   d: del   q: quit", 14, H - 16, 0x707888);
            sys_gfx_blit(FB);
            dirty = 0;
        }
        int k = sys_pollkey();
        if (editing) {
            if (k == '\n' || k == '\r') {
                if (editing == 2) { if (elen > 0) { for (int j = 0; j <= elen; j++) items[sel].t[j] = eb[j]; save(); } }   /* edit the selected item */
                else if (elen > 0 && nit < MAXIT) { for (int j = 0; j <= elen; j++) items[nit].t[j] = eb[j]; items[nit].done = 0; sel = nit; nit++; save(); }   /* add a new item */
                editing = 0; dirty = 1;
            }
            else if (k == 27) { editing = 0; dirty = 1; }
            else if (k == 8 || k == 0x7F) { if (elen > 0) eb[--elen] = 0; dirty = 1; }
            else if (k >= ' ' && k < 127 && elen < TLEN - 1) { eb[elen++] = (char)k; eb[elen] = 0; dirty = 1; }
        } else {
            if (k == 'q' || k == 27) break;
            else if (k == 'a' || k == 'A') { editing = 1; elen = 0; eb[0] = 0; dirty = 1; }
            else if ((k == 'e' || k == 'E') && nit > 0) { editing = 2; elen = 0; for (int j = 0; items[sel].t[j] && elen < TLEN - 1; j++) eb[elen++] = items[sel].t[j]; eb[elen] = 0; dirty = 1; }
            else if ((k == 0x11 || k == 'w') && sel > 0) { sel--; dirty = 1; }
            else if ((k == 0x12 || k == 's') && sel < nit - 1) { sel++; dirty = 1; }
            else if (k == ' ' && nit > 0) { items[sel].done = !items[sel].done; save(); dirty = 1; }
            else if ((k == 'd' || k == 'D') && nit > 0) { for (int j = sel; j < nit - 1; j++) items[j] = items[j + 1]; nit--; if (sel >= nit) sel = nit > 0 ? nit - 1 : 0; save(); dirty = 1; }
        }
        sys_sleep(40);
    }
    return 0;
}
