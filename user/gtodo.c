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

/* ---- instrument-panel UI kit (M1431; see gconv.c M1430) ---- */
#define C_FACE_T  0x232D33u
#define C_FACE_B  0x161D21u
#define C_BEZHI   0x3C4A50u
#define C_BEZLO   0x0E1316u
#define C_SCREEN  0x0A0F0Cu
#define C_SCANLN  0x0D140Fu
#define C_AMBER   0xFFB23Eu
#define C_AMBERLO 0x7A521Au
#define C_LABEL   0xC6D0CCu
#define C_DIM     0x6E827Fu
#define C_LED     0x46E0A0u
#define C_SELBAR  0x16221Cu

static void vgrad(int x, int y, int w, int h, unsigned t, unsigned b) {
    for (int r = 0; r < h; r++) {
        int R = ((int)(t >> 16 & 0xFF) * (h - 1 - r) + (int)(b >> 16 & 0xFF) * r) / (h - 1);
        int G = ((int)(t >> 8  & 0xFF) * (h - 1 - r) + (int)(b >> 8  & 0xFF) * r) / (h - 1);
        int B = ((int)(t       & 0xFF) * (h - 1 - r) + (int)(b       & 0xFF) * r) / (h - 1);
        fill(x, y + r, w, 1, ((unsigned)R << 16) | ((unsigned)G << 8) | (unsigned)B);
    }
}
static void bevel_up(int x, int y, int w, int h, unsigned hi, unsigned lo) {
    fill(x, y, w, 1, hi); fill(x, y, 1, h, hi); fill(x, y + h - 1, w, 1, lo); fill(x + w - 1, y, 1, h, lo);
}
static void bevel_dn(int x, int y, int w, int h, unsigned hi, unsigned lo) {
    fill(x, y, w, 1, lo); fill(x, y, 1, h, lo); fill(x, y + h - 1, w, 1, hi); fill(x + w - 1, y, 1, h, hi);
}
static void panel(int x, int y, int w, int h) {
    fill(x, y, w, h, C_SCREEN);
    for (int r = 3; r < h - 1; r += 3) fill(x + 1, y + r, w - 2, 1, C_SCANLN);
    bevel_dn(x, y, w, h, C_BEZHI, C_BEZLO);
}
static void led(int x, int y) {
    fill(x, y, 9, 9, C_BEZLO); fill(x + 1, y + 1, 7, 7, 0x1A6E50u);
    fill(x + 2, y + 2, 5, 5, C_LED); putpx(x + 3, y + 3, 0xCFFFE8u);
}
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
            vgrad(0, 0, W, H, C_FACE_T, C_FACE_B);                 /* slate faceplate */
            bevel_up(0, 0, W, H, C_BEZHI, C_BEZLO);
            text("TO-DO LIST", 14, 10, C_LABEL);
            fill(14, 27, 84, 2, C_AMBERLO);
            led(W - 24, 11);
            { char c[16]; int p = 0, done = 0;
              for (int k = 0; k < nit; k++) if (items[k].done) done++;
              p = putdec(c, p, done); c[p++] = '/'; p = putdec(c, p, nit); c[p] = 0;
              text(c, W - 40 - p * 8, 10, C_DIM); }

            int sy = 34, sh = H - 34 - 22;                         /* recessed list screen */
            panel(8, sy, W - 16, sh);
            if (nit == 0 && !editing) text("(empty - press a to add an item)", 20, sy + 12, C_DIM);
            for (int k = 0; k < nit; k++) {
                int y = sy + 10 + k * 22;
                if (y + 18 > sy + sh) break;                       /* clip to the screen */
                if (k == sel && !editing) { fill(10, y - 2, W - 20, 20, C_SELBAR); fill(10, y - 2, 2, 20, C_AMBER); }   /* lit row + amber edge */
                text(items[k].done ? "[x]" : "[ ]", 18, y, items[k].done ? C_LED : C_AMBERLO);
                text(items[k].t, 54, y, items[k].done ? C_DIM : C_LABEL);
            }
            if (editing) {                                  /* the new-item input line */
                int ey = sy + 10 + nit * 22 + 4; if (ey > sy + sh - 22) ey = sy + sh - 22;
                text(editing == 2 ? "Edit:" : "New:", 18, ey, C_LED);
                text(eb, 58, ey, C_AMBER);
                fill(58 + elen * 8, ey, 8, 16, C_AMBERLO);  /* caret */
            }
            text(editing ? "Enter save    Esc cancel" : "a add  e edit  v paste  space done  d del  c clear  q quit", 14, H - 14, C_DIM);
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
            else if ((k == 'v' || k == 'V') && nit < MAXIT) {   /* paste the clipboard's first line as a new item */
                char cb[TLEN]; int cn = sys_clip_get(cb, TLEN - 1);
                int p = 0; for (int i = 0; i < cn && cb[i] && cb[i] != '\n' && p < TLEN - 1; i++) items[nit].t[p++] = cb[i];
                items[nit].t[p] = 0;
                if (p > 0) { items[nit].done = 0; sel = nit; nit++; save(); dirty = 1; }
            }
            else if ((k == 'e' || k == 'E') && nit > 0) { editing = 2; elen = 0; for (int j = 0; items[sel].t[j] && elen < TLEN - 1; j++) eb[elen++] = items[sel].t[j]; eb[elen] = 0; dirty = 1; }
            else if ((k == 0x11 || k == 'w') && sel > 0) { sel--; dirty = 1; }
            else if ((k == 0x12 || k == 's') && sel < nit - 1) { sel++; dirty = 1; }
            else if (k == ' ' && nit > 0) { items[sel].done = !items[sel].done; save(); dirty = 1; }
            else if ((k == 'd' || k == 'D') && nit > 0) { for (int j = sel; j < nit - 1; j++) items[j] = items[j + 1]; nit--; if (sel >= nit) sel = nit > 0 ? nit - 1 : 0; save(); dirty = 1; }
            else if ((k == 'c' || k == 'C') && nit > 0) { int wr = 0; for (int r = 0; r < nit; r++) if (!items[r].done) items[wr++] = items[r]; nit = wr; if (sel >= nit) sel = nit > 0 ? nit - 1 : 0; save(); dirty = 1; }   /* clear completed */
        }
        sys_sleep(40);
    }
    return 0;
}
