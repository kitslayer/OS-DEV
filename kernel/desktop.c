/*
 * desktop.c — the window manager / compositor + desktop apps.
 *
 * Compositor: every frame the whole scene is redrawn into an off-screen back
 * buffer (wallpaper -> windows back-to-front -> start menu -> cursor) and
 * blitted at once (fb_present), so overlap never flickers. Stacking is array
 * order; windows[count-1] is on top. Title bar moves a window, the corner grip
 * resizes it, the red box closes it, and the Apps menu launches more.
 *
 * Windows come in two flavours: kernel-drawn info windows (Welcome, Files,
 * Clock, About) and **app windows** (KIND_APP) that host a real ring-3
 * userspace process (see app.c) — its text grid is drawn here and keystrokes
 * are delivered to it when it's focused.
 */
#include "desktop.h"
#include "fb.h"
#include "speaker.h"
#include "font.h"
#include "mouse.h"
#include "usb.h"
#include "usb_kbd.h"
#include "kheap.h"
#include "keyboard.h"
#include "timer.h"
#include "app.h"
#include "browser.h"
#include "acpi.h"
#include "net.h"
#include "vfs.h"
#include "rtc.h"
#include "image.h"   /* decode_image(): any format the OS decodes (PNG/BMP/JPEG/GIF/SVG) */
#include "string.h"
#include "pmm.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_WINDOWS 16
#define TITLEBAR_H  26
#define TASKBAR_H   34
#define GRIP        16

enum { KIND_PLAIN, KIND_WELCOME, KIND_FILES, KIND_APP, KIND_CLOCK, KIND_ABOUT,
       KIND_BROWSER, KIND_SYSMON, KIND_POWEROFF, KIND_REBOOT };  /* power actions: not windows, they call ACPI */

typedef struct {
    int x, y, w, h;
    uint32_t body;
    const char *title;
    int kind;
    void *app;            /* app_t* for KIND_APP windows */
    int maximized;        /* F4 toggle: filling the screen */
    int sx, sy, sw, sh;   /* saved geometry to restore from maximize */
    int fsel;             /* KIND_FILES: selected row (keyboard nav) */
    int fconfirm;         /* KIND_FILES: a delete is armed, awaiting a 2nd d/y (else any key cancels) */
    int editing;          /* KIND_FILES: 0=none, 1=rename, 2=new-folder (a text-input modal) */
    char editbuf[16];     /* the typed name (8.3 = max 12 chars + NUL) */
    int editlen;          /* chars in editbuf */
    int minimized;        /* F3: hidden to its taskbar chip (last field; the
                             positional struct literals below init every field) */
} window_t;

static window_t windows[MAX_WINDOWS];
static int win_count;
static uint32_t *backbuffer;
static uint32_t *scenebuf;          /* cached rendered scene (no cursor) */
static int screen_w, screen_h;
static int spawn_n, menu_open, menu_sel;   /* menu_sel: keyboard-highlighted item */
static int help_open;                       /* F1: keyboard-shortcut help overlay */
static int sw_open, sw_sel;                 /* F7: Alt-Tab-style window switcher overlay (sw_sel: highlighted window) */
static int ctx_open, ctx_x, ctx_y, ctx_win; /* right-click menu (window kind: ctx_win is always the topmost window, win_count-1) */
static int ctx_kind;                        /* 0 = window title-bar menu (ctx_win), 1 = desktop-background menu */
/* Close every modal overlay. Called by each overlay's open path so at most one is
 * ever up at a time (the open paths are otherwise asymmetric — e.g. F1 is handled
 * before the switcher's modal key-swallow, so F1-while-switcher would leave both
 * open; a right-click while a menu is up would stack a context menu on top). */
static void close_overlays(void) { menu_open = help_open = sw_open = ctx_open = 0; }
static int start_x = 8, start_y, start_w = 110, start_h = 24;

struct menu_item { const char *label; int kind; const char *prog; };
static const struct menu_item menu[] = {
    { "Browser", KIND_BROWSER, 0 }, { "Shell", KIND_APP, "shell" },
    { "Clock", KIND_APP, "clock" }, { "Analog Clock", KIND_APP, "aclock" }, { "Calc", KIND_APP, "calc" },
    { "System Monitor", KIND_APP, "sysgraph" },
    { "Snake", KIND_APP, "snake" }, { "Editor", KIND_APP, "editor" },
    { "2048", KIND_APP, "2048" }, { "Life", KIND_APP, "life" }, { "Mines", KIND_APP, "mines" },
    { "Tetris", KIND_APP, "tetris" }, { "Breakout", KIND_APP, "breakout" },
    { "Sudoku", KIND_APP, "sudoku" }, { "Maze", KIND_APP, "maze" }, { "Mandelbrot", KIND_APP, "mandel" },
    { "Hangman", KIND_APP, "hangman" }, { "Adventure", KIND_APP, "adv" },
    { "Tic-Tac-Toe", KIND_APP, "ttt" }, { "Blackjack", KIND_APP, "bj" }, { "Typing", KIND_APP, "typing" },
    { "Simon", KIND_APP, "simon" }, { "Connect 4", KIND_APP, "c4" },
    { "Wordle", KIND_APP, "wordle" },
    { "Graphics Demo", KIND_APP, "gfxdemo" },
    { "3D Engine", KIND_APP, "scene3d" },
    { "Terrain", KIND_APP, "terrain" },
    { "Demoscene", KIND_APP, "demoscene" },
    { "DOOM", KIND_APP, "doom" },
    { "Quake", KIND_APP, "quake" },
    { "NES", KIND_APP, "nes" },
    { "Reversi", KIND_APP, "reversi" },
    { "Lights Out", KIND_APP, "lights" },
    { "15 Puzzle", KIND_APP, "fifteen" },
    { "Mastermind", KIND_APP, "mastermind" },
    { "Pong", KIND_APP, "pong" },
    { "Half-Life", KIND_APP, "halflife" },
    { "Memory", KIND_APP, "memory" },
    { "Sokoban", KIND_APP, "sokoban" },
    { "Battleship", KIND_APP, "battleship" },
    { "Pig", KIND_APP, "pig" },
    { "Raycaster", KIND_APP, "raycast" },
    { "Tron", KIND_APP, "tron" },
    { "Space Invaders", KIND_APP, "spaceinv" },
    { "Asteroids", KIND_APP, "asteroids" },
    { "Flappy", KIND_APP, "flappy" },
    { "Game Boy", KIND_APP, "gb" },
    { "Lunar Lander", KIND_APP, "lander" },
    { "Yahtzee", KIND_APP, "yahtzee" },
    { "Checkers", KIND_APP, "checkers" },
    { "Gomoku", KIND_APP, "gomoku" },
    { "Frogger", KIND_APP, "frogger" },
    { "Chess", KIND_APP, "chess" },
    { "Video Poker", KIND_APP, "vpoker" },
    { "Mancala", KIND_APP, "mancala" },
    { "Dots and Boxes", KIND_APP, "dotsbox" },
    { "Missile Command", KIND_APP, "missile" },
    { "Pac-Man", KIND_APP, "pacman" },
    { "Solitaire", KIND_APP, "solitaire" },
    { "Gems", KIND_APP, "gems" },
    { "Columns", KIND_APP, "columns" },
    { "FreeCell", KIND_APP, "freecell" },
    { "Spider", KIND_APP, "spider" },
    { "Paint", KIND_APP, "paint" }, { "Piano", KIND_APP, "piano" }, { "Jukebox", KIND_APP, "jukebox" },
    { "Matrix", KIND_APP, "matrix" }, { "Calendar", KIND_APP, "calendar" },
    { "Timer", KIND_APP, "timer" },
    { "Sandbox", KIND_APP, "sandbox" },
    { "Forth", KIND_APP, "forth" },
    { "Hex Edit", KIND_APP, "hexedit" },
    { "Monitor", KIND_SYSMON, 0 },
    { "Files", KIND_FILES, 0 }, { "Welcome", KIND_WELCOME, 0 },
    { "About", KIND_ABOUT, 0 },
    { "Restart", KIND_REBOOT, 0 }, { "Shut Down", KIND_POWEROFF, 0 },
};
/* The Apps menu is laid out in 2 columns rendered upward from the taskbar, so
 * MENU_PERCOL*MENU_ITEM_H must fit the screen height (17*24+4 = 412px << 734).
 * That holds ~2*30 = 60 entries before the per-column height clips. */
#define MENU_N      (int)(sizeof(menu) / sizeof(menu[0]))
#define MENU_W      150
#define MENU_ITEM_H 24
#define MENU_COLS   3
#define MENU_PERCOL ((MENU_N + MENU_COLS - 1) / MENU_COLS)
#define TB_CHIPW    124                 /* taskbar window-chip width */
#define TB_CHIPGAP  6
#define TB_CHIPX0   (start_x + start_w + 10)
/* Right-click context menus, sized like a slim Apps menu. Two kinds share the same
 * popup machinery (ctx_kind): the window title-bar menu (5 rows; row 0's label is
 * Maximize or Restore depending on the window's `maximized` flag) and the desktop-
 * background menu (3 rows). Row count + width are per-kind via ctx_nrows()/ctx_w();
 * the row height is shared. */
#define CTX_ROWS    5                       /* window menu */
#define CTX_W       128                     /* window menu */
#define CTX_DESK_ROWS 3                     /* desktop menu: Show Desktop / Show All Windows / Change Wallpaper */
#define CTX_DESK_W  160                     /* desktop menu: wider so "Show All Windows" fits */
#define CTX_ROW_H   22

static void draw_text(int x, int y, const char *s, uint32_t fg) {
    for (int i = 0; s[i]; i++)
        fb_glyph_fg(x + i * font_width, y, s[i], fg);
}
/* file-type tint for the Files list (RGB, readable on the light rows); parallels
 * the shell's ls_color. FAT32 8.3 names are upper-case, so compare upper-case. M1332 */
static int ext_is(const char *x, const char *s) {
    int i = 0; for (; x[i] && s[i]; i++) if (x[i] != s[i]) return 0;
    return x[i] == 0 && s[i] == 0;
}
static uint32_t file_color(const char *name) {
    int dot = -1; for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
    if (dot < 0) return 0x303840;
    const char *x = name + dot + 1;
    if (ext_is(x,"SVG")||ext_is(x,"PNG")||ext_is(x,"BMP")||ext_is(x,"GIF")||ext_is(x,"JPG")||ext_is(x,"JPE")) return 0x8E24AA; /* images: purple */
    if (ext_is(x,"C")||ext_is(x,"H")||ext_is(x,"JS"))                       return 0xB05A00; /* code: orange-brown */
    if (ext_is(x,"GZ")||ext_is(x,"TAR")||ext_is(x,"TGZ")||ext_is(x,"ZIP"))  return 0xC01010; /* archives: red */
    if (ext_is(x,"WAV"))                                                    return 0x00838A; /* audio: teal */
    if (ext_is(x,"ELF")||ext_is(x,"SH"))                                    return 0x2E7D32; /* executables: green */
    if (ext_is(x,"NES")||ext_is(x,"GB"))                                    return 0xC0006A; /* ROMs: pink */
    return 0x303840;                                                                         /* text/web/default */
}
static void box(int x, int y, int w, int h, uint32_t c) {
    fb_fill_rect(x, y, w, 1, c); fb_fill_rect(x, y + h - 1, w, 1, c);
    fb_fill_rect(x, y, 1, h, c); fb_fill_rect(x + w - 1, y, 1, h, c);
}

/* --- theming helpers --- */
static uint32_t lerp(uint32_t a, uint32_t b, int n, int d) {
    if (d <= 0) d = 1;
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r=ar+(br-ar)*n/d, g=ag+(bg-ag)*n/d, bl=ab+(bb-ab)*n/d;
    return (uint32_t)(r<<16 | g<<8 | bl);
}
static void vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    for (int j = 0; j < h; j++)
        fb_fill_rect(x, y + j, w, 1, lerp(top, bot, j, h - 1));
}
/* Darken whatever is already in the back buffer (for soft shadows). */
static void darken(int x, int y, int w, int h, int pct) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t p = fb_get_pixel(x + i, y + j);
            uint32_t r=((p>>16)&0xFF)*pct/100, g=((p>>8)&0xFF)*pct/100, b=(p&0xFF)*pct/100;
            fb_pixel(x + i, y + j, r<<16 | g<<8 | b);
        }
}
/* Corner-pixel inset per row from the corner (index 0 = outermost row): a
 * quarter-circle of radius 8 for noticeably rounder, more modern window corners. */
static const int corner[] = { 8, 5, 3, 2, 2, 1, 1, 1 };
#define CORNER_R ((int)(sizeof(corner)/sizeof(corner[0])))

/* Round the 4 corners of a small chrome element (taskbar chip/button, close box)
 * by repainting the corner pixels with the panel behind it — sampled from that
 * panel's vertical gradient (bg0 at panel_y .. bg1 at panel_y+panel_h-1) — so the
 * taskbar furniture and close button echo the windows' rounded corners for one
 * cohesive look. A 2px quarter-circle: subtle at chip scale, but it kills the
 * boxy hard corners. */
static const int corner2[] = { 2, 1 };
#define CORNER2_R ((int)(sizeof(corner2)/sizeof(corner2[0])))
static void round_chrome(int x, int y, int w, int h, uint32_t bg0, uint32_t bg1, int panel_y, int panel_h) {
    if (panel_h <= 1) panel_h = 2;
    for (int j = 0; j < CORNER2_R; j++) {
        int in = corner2[j];
        uint32_t ct = lerp(bg0, bg1, (y + j) - panel_y, panel_h - 1);
        uint32_t cb = lerp(bg0, bg1, (y + h - 1 - j) - panel_y, panel_h - 1);
        for (int i = 0; i < in; i++) {
            fb_pixel(x + i, y + j, ct);
            fb_pixel(x + w - 1 - i, y + j, ct);
            fb_pixel(x + i, y + h - 1 - j, cb);
            fb_pixel(x + w - 1 - i, y + h - 1 - j, cb);
        }
    }
}

#define WP_TOP 0x183A5C
#define WP_BOT 0x081320
static int wp_h;
static uint32_t *wallpaper_bmp;   /* a screen-sized image loaded from disk, or NULL = gradient */
static volatile int wallpaper_repaint;   /* set by desktop_set_wallpaper (off-task) to force a redraw */
/* Background colour at (x,y): the loaded wallpaper if present, else the gradient. */
static uint32_t wallpaper_at(int x, int y) {
    if (wallpaper_bmp && x >= 0 && x < screen_w && y >= 0 && y < screen_h)
        return wallpaper_bmp[(size_t)y * screen_w + x];
    int yy = y < 0 ? 0 : (y >= wp_h ? wp_h - 1 : y);
    return lerp(WP_TOP, WP_BOT, yy, wp_h - 1);
}
static void u2(uint64_t v, char *o) { o[0]='0'+(v/10)%10; o[1]='0'+v%10; o[2]=0; }
static int lc_ascii(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }   /* ASCII lowercase */
/* Width of the taskbar clock pill ("YYYY-MM-DD  HH:MM:SS" = 20 chars + padding).
 * Used by BOTH the renderer and the chip hit-test so they agree on where the
 * clock starts (otherwise a click in the clock area mis-hits a window chip). */
static int clk_pill_w(void) { return 20 * font_width + 26; }
static int unum(uint64_t v, char *o) {           /* general unsigned -> string; returns len */
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) o[j++] = t[--i];
    o[j] = 0; return j;
}

static void win_min(const window_t *w, int *mw, int *mh) {
    if (w->kind == KIND_APP) {
        *mw = app_cols() * font_width + 14;
        *mh = app_rows() * font_height + TITLEBAR_H + 14;
    } else if (w->kind == KIND_BROWSER) { *mw = 340; *mh = 240; }
    else if (w->kind == KIND_SYSMON)  { *mw = 320; *mh = 272; }  /* a fixed-layout info panel (= its open size): can't be shrunk below its Memory/Network/Disk content, which would otherwise draw past the bottom edge */
    else if (w->kind == KIND_WELCOME) { *mw = 360; *mh = 290; }  /* likewise a fixed-layout panel pinned to its open size */
    else if (w->kind == KIND_ABOUT)   { *mw = 300; *mh = 178; }  /* likewise */
    else { *mw = 170; *mh = 110; }
}

/* Integer upscale for a small graphics canvas, so e.g. DOOM's native 320x200
 * shows in a comfortable 640x400 window (crisp nearest-neighbour, and it keeps
 * DOOM on its correct 1:1 render path rather than its buggy internal scaler). */
static int gfx_scale(int gw, int gh) {
    int s = 1;
    while ((s + 1) * gw <= 700 && (s + 1) * gh <= 520) s++;
    return s;
}

static void draw_content(const window_t *w, int focused) {
    int bx = w->x + 8, by = w->y + TITLEBAR_H + 8;
    switch (w->kind) {
    case KIND_WELCOME: {
        const char *L[] = { "Welcome to OS-DEV!", "",
            "A from-scratch x86_64 OS: kernel, FAT32",
            "disk, desktop and apps, all hand-built.", "",
            "Open the Apps menu (F9) and try:",
            "- Browser: the real web over HTTPS + JS",
            "- Shell: scriptable (functions, loops, $())",
            "- Editor: undo/redo, find & replace",
            "- 60+ apps, games and demos", "",
            "Drag the title bar or corner to move.",
            "F9 = Apps menu     F1 = all shortcuts" };
        for (unsigned i = 0; i < sizeof(L)/sizeof(L[0]); i++) {           /* colourised intro (M1333) */
            const char *s = L[i];
            if (s[0] == '-' && s[1] == ' ') {                            /* app bullet: "- Label:" accent, rest dark */
                int colon = -1; for (int k = 0; s[k]; k++) if (s[k] == ':') { colon = k; break; }
                if (colon > 0) {
                    char seg[48]; int p = 0; for (int k = 0; k <= colon && p < 47; k++) seg[p++] = s[k]; seg[p] = 0;
                    draw_text(bx, by + i*18, seg, 0x1F56C6);
                    draw_text(bx + (colon + 1)*font_width, by + i*18, s + colon + 1, 0x202028);
                    continue;
                }
            }
            uint32_t col = (i == 0) ? 0x1F56C6                           /* heading accent blue */
                         : (s[0] == 'F' && s[1] == '9') ? 0x4A6FA8       /* shortcut hint: muted blue */
                         : 0x202028;                                     /* body dark */
            draw_text(bx, by + i*18, s, col);
        }
        fb_fill_rect(bx, by + 16, w->w - 24, 1, 0xC9CCD6);                /* accent rule under the heading */
        break;
    }
    case KIND_FILES: {
        static vfs_dirent e[256]; int n = vfs_list(e, 256);   /* static (BSS): ~18KB won't fit the 16KB guard-page-less stack; single-threaded render makes it safe. Browse ALL disk files, not just the first 32 (M421) */
        if (w->editing) {                                      /* a text-input is open: prompt + the typed name + a cursor */
            char pr[48]; int p = 0;
            const char *a = w->editing == 1 ? "Rename to: " : "New folder: ";
            while (*a && p < (int)sizeof(pr) - 1) pr[p++] = *a++;
            for (int j = 0; w->editbuf[j] && p < (int)sizeof(pr) - 2; j++) pr[p++] = w->editbuf[j];
            pr[p++] = '_';                                     /* a simple text cursor */
            pr[p] = 0;
            draw_text(bx, by, pr, 0x1060C0);                   /* distinct blue: an input prompt, not the file list */
        } else if (w->fconfirm && w->fsel >= 0 && w->fsel < n) {  /* a delete is armed: replace the header with a bright confirm prompt */
            char pr[64]; int p = 0; const char *a = "Delete ";
            while (*a) pr[p++] = *a++;
            for (int j = 0; e[w->fsel].name[j] && p < 28; j++) pr[p++] = e[w->fsel].name[j];
            const char *b = "?  d/y=confirm  any key=cancel";
            for (int j = 0; b[j] && p < (int)sizeof(pr) - 1; j++) pr[p++] = b[j];
            pr[p] = 0;
            draw_text(bx, by, pr, 0xC01010);                   /* bright red: this action destroys a file */
        } else {
            draw_text(bx, by, "FAT32 (/) up/down Enter open  d del  r rename  n new-folder  w wallpaper", 0x202028);
        }
        fb_fill_rect(bx - 2, by + 17, w->w - 14, 1, 0xC4CAD6);   /* rule under the header */
        int rows = (w->h - TITLEBAR_H - 30) / 18;          /* rows that fit in the body */
        if (rows < 1) rows = 1;
        int top = 0;                                       /* scroll so the selection stays visible */
        if (w->fsel >= rows) top = w->fsel - rows + 1;
        for (int i = top; i < n && i < top + rows; i++) {
            int ry = by + 22 + (i - top)*18;
            if (i == w->fsel)                              /* highlight the selected row */
                fb_fill_rect(bx - 2, ry - 2, w->w - 14, 18, 0xCFE0F5);
            else if ((i - top) & 1)                        /* zebra striping for scanability */
                fb_fill_rect(bx - 2, ry - 2, w->w - 14, 18, 0xDFE5F0);
            int nl = 0; while (e[i].name[nl]) nl++;
            int isdir = (nl > 0 && e[i].name[nl-1] == '/');   /* vfs marks directories with a trailing '/' */
            char line[48]; int p = 0; line[p++]=' '; line[p++]=' ';
            for (int j = 0; e[i].name[j] && p < 28; j++) line[p++] = e[i].name[j];
            int name_end = p;                              /* end of the "  NAME" segment (M1332) */
            while (p < 22) line[p++] = ' ';
            if (isdir) {                                   /* directory: show "(dir)", no size/date */
                const char *d = "(dir)"; for (int z = 0; d[z]; z++) line[p++] = d[z];
            } else {
                line[p++]='('; uint32_t s=e[i].size; char num[12]; int k=0;
                if (!s) num[k++]='0';
                while (s) { num[k++]='0'+s%10; s/=10; }
                while (k) line[p++]=num[--k];
                line[p++]='b'; line[p++]=')';
                if (e[i].date && p < 36) {                 /* last-write date (YYYY-MM-DD) for stamped files */
                    int yr=(e[i].date>>9)+1980, mo=(e[i].date>>5)&15, dy=e[i].date&31;
                    line[p++]=' ';
                    line[p++]='0'+(yr/1000)%10; line[p++]='0'+(yr/100)%10; line[p++]='0'+(yr/10)%10; line[p++]='0'+yr%10;
                    line[p++]='-'; line[p++]='0'+(mo/10)%10; line[p++]='0'+mo%10;
                    line[p++]='-'; line[p++]='0'+(dy/10)%10; line[p++]='0'+dy%10;
                }
            }
            line[p]=0;
            uint32_t namecol = isdir ? 0x8A5A00 : file_color(e[i].name);   /* dirs gold, files by type (M1332) */
            char nsave = line[name_end]; line[name_end] = 0;
            draw_text(bx, ry, line, namecol);                              /* name in its type tint */
            line[name_end] = nsave;
            draw_text(bx + name_end * font_width, ry, line + name_end, 0x808890);   /* size/date in grey */
        }
        break;
    }
    case KIND_APP: {
        uint32_t *cb; int gw, gh;
        if (w->app && app_gfx_get((app_t *)w->app, &cb, &gw, &gh)) {
            /* graphics mode: blit the app's pixel canvas into the body, scaled
             * up by an integer factor (nearest-neighbour) for small canvases. */
            int s = gfx_scale(gw, gh), dx = bx - 2, dy = by - 2;
            for (int yy = 0; yy < gh; yy++)
                for (int xx = 0; xx < gw; xx++) {
                    uint32_t px = cb[yy * gw + xx] & 0xFFFFFF;
                    if (s == 1) { fb_pixel(dx + xx, dy + yy, px); continue; }
                    for (int oy = 0; oy < s; oy++)
                        for (int ox = 0; ox < s; ox++)
                            fb_pixel(dx + xx * s + ox, dy + yy * s + oy, px);
                }
        } else if (w->app) app_render((app_t *)w->app, bx - 2, by - 2, focused);
        break;
    }
    case KIND_BROWSER:
        if (w->app) browser_render((browser_t *)w->app, w->x, w->y + TITLEBAR_H,
                                   w->w, w->h - TITLEBAR_H);
        break;
    case KIND_CLOCK: {
        uint64_t sec = timer_ticks() / 100;
        char t[6]; u2(sec/60, t); t[2]=':'; u2(sec%60, t+3);
        fb_text(w->x + 28, by + 18, t, 0x40FF90, 5);
        break;
    }
    case KIND_SYSMON: {
        uint64_t tot = pmm_total_bytes(), fre = pmm_free_bytes();
        uint64_t used = tot > fre ? tot - fre : 0;
        int barw = w->w - 32, pct = tot ? (int)(used * 100 / tot) : 0;
        char line[64];

        draw_text(bx, by, "Memory", 0x202028);
        int yb = by + 18;
        fb_fill_rect(bx, yb, barw, 14, 0xDADEE6);                    /* bar track */
        uint32_t bc = pct < 70 ? 0x3CB371 : (pct < 90 ? 0xE0A030 : 0xD64545);
        fb_fill_rect(bx, yb, barw * pct / 100, 14, bc);              /* used */
        box(bx, yb, barw, 14, 0x9098A4);
        int p = 0;
        p += unum(used/(1024*1024), line+p); line[p++]=' '; line[p++]='/'; line[p++]=' ';
        p += unum(tot/(1024*1024), line+p);
        const char *u = " MiB used"; for (int i=0;u[i];i++) line[p++]=u[i]; line[p]=0;
        draw_text(bx, yb + 20, line, 0x303840);

        draw_text(bx, yb + 46, "Tasks", 0x202028);
        int nt = task_count();
        for (int i = 0; i < nt && i < 20; i++)                        /* one block per task */
            fb_fill_rect(bx + 60 + i*10, yb + 46, 7, 11, 0x4A90E2);
        p = 0; p += unum((uint64_t)nt, line+p); line[p]=0;
        draw_text(bx + 60 + (nt<20?nt:20)*10 + 6, yb + 46, line, 0x303840);

        char up[40]; p = 0;
        const char *uh = "Uptime "; for (int i=0;uh[i];i++) up[p++]=uh[i];
        p += unum(timer_ticks()/100, up+p); up[p++]='s'; up[p]=0;
        draw_text(bx, yb + 72, up, 0x303840);

        /* network: our IP + gateway (a connected, internet-capable OS) */
        draw_text(bx, yb + 98, "Network", 0x202028);
        const uint8_t *ip = net_ip(), *gw = net_gateway();
        char net[64]; p = 0;
        const char *ih = "IP "; for (int i=0;ih[i];i++) net[p++]=ih[i];
        for (int k=0;k<4;k++){ p+=unum(ip[k],net+p); if(k<3)net[p++]='.'; }
        const char *gh = "  gw "; for (int i=0;gh[i];i++) net[p++]=gh[i];
        for (int k=0;k<4;k++){ p+=unum(gw[k],net+p); if(k<3)net[p++]='.'; }
        net[p]=0;
        draw_text(bx, yb + 116, net, 0x303840);

        /* Disk (FAT32 volume). vfs_df() scans the whole FAT (disk I/O), so cache
         * it and refresh only every ~5s rather than on every render frame. */
        static uint64_t df_free, df_total, df_at = (uint64_t)-1;
        uint64_t nowt = timer_ticks();
        if (df_at == (uint64_t)-1 || nowt - df_at > 500) { vfs_df(&df_free, &df_total); df_at = nowt; }
        draw_text(bx, yb + 142, "Disk", 0x202028);
        if (df_total) {
            uint64_t dused = df_total > df_free ? df_total - df_free : 0;
            int dpct = (int)(dused * 100 / df_total), yd = yb + 160;
            fb_fill_rect(bx, yd, barw, 14, 0xDADEE6);
            uint32_t dc = dpct < 70 ? 0x3CB371 : (dpct < 90 ? 0xE0A030 : 0xD64545);
            fb_fill_rect(bx, yd, barw * dpct / 100, 14, dc);
            box(bx, yd, barw, 14, 0x9098A4);
            p = 0;
            p += unum(dused/(1024*1024), line+p); line[p++]=' '; line[p++]='/'; line[p++]=' ';
            p += unum(df_total/(1024*1024), line+p);
            const char *du = " MiB used"; for (int i=0;du[i];i++) line[p++]=du[i]; line[p]=0;
            draw_text(bx, yd + 20, line, 0x303840);
        } else {
            draw_text(bx, yb + 160, "no disk", 0x303840);
        }
        break;
    }
    case KIND_ABOUT: {
        const char *L[] = { "OS-DEV", "", "x86_64, built from scratch", "",
            "kernel . memory . tasks",
            "FAT32 . TCP . TLS 1.3",
            "JS engine . web browser",
            "scriptable shell . editor" };
        for (unsigned i = 0; i < sizeof(L)/sizeof(L[0]); i++)
            draw_text(bx, by + i*16, L[i], (i == 0 || i >= 4) ? 0x1F56C6 : 0x202028);   /* heading + tech-stack lines in accent blue (M1334) */
        fb_fill_rect(bx, by + 14, w->w - 24, 1, 0xC9CCD6);                  /* rule under the heading */
        break;
    }
    default:
        draw_text(bx, by, w->title, 0x303840);
        break;
    }
}

/* A filled circle (no FPU; integer radius test) for the icon glyphs. */
static void fcircle(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r) fb_pixel(cx + dx, cy + dy, c);
}
/* A small per-kind icon (ICON_SZ square) for the title bar + taskbar chip, so a
 * window is identifiable at a glance. Clean colored glyphs that read on both the
 * title-bar gradient and the dark taskbar. */
#define ICON_SZ 14
static void draw_icon(int kind, int x, int y) {
    int cx = x + 7, cy = y + 7;
    switch (kind) {
    case KIND_FILES:
        fb_fill_rect(x + 1, y + 3, 6, 2, 0xC99A3A);          /* folder tab */
        fb_fill_rect(x + 1, y + 4, 12, 8, 0xF0C674);         /* folder body */
        fb_fill_rect(x + 1, y + 4, 12, 1, 0xFFE6B0);         /* top highlight */
        break;
    case KIND_BROWSER:
        fcircle(cx, cy, 6, 0x68B6E6);                        /* globe */
        fb_fill_rect(x + 1, cy, 12, 1, 0xEAF4FF);            /* equator */
        fb_fill_rect(cx, y + 1, 1, 12, 0xEAF4FF);            /* meridian */
        break;
    case KIND_CLOCK:
        fcircle(cx, cy, 6, 0xF2F2F2);                        /* clock face */
        fb_fill_rect(cx, cy - 4, 1, 5, 0x2A2A33);            /* minute hand */
        fb_fill_rect(cx, cy, 4, 1, 0x2A2A33);                /* hour hand */
        break;
    case KIND_SYSMON:
        fb_fill_rect(x + 2,  y + 8, 2, 4, 0x3CB371);         /* bar chart */
        fb_fill_rect(x + 6,  y + 5, 2, 7, 0xE0A030);
        fb_fill_rect(x + 10, y + 3, 2, 9, 0x6BB0FF);
        break;
    case KIND_WELCOME:
    case KIND_ABOUT:
        fcircle(cx, cy, 6, 0xF2F2F2);                        /* info badge */
        fb_fill_rect(cx, y + 3, 1, 2, 0x2C66D6);             /* i dot */
        fb_fill_rect(cx, y + 6, 1, 5, 0x2C66D6);             /* i stem */
        break;
    default:                                                 /* KIND_APP etc.: a window glyph */
        fb_fill_rect(x + 1, y + 2, 12, 10, 0xE8ECF4);
        fb_fill_rect(x + 1, y + 2, 12, 3, 0x33415A);
        box(x + 1, y + 2, 12, 10, 0x2A3344);
        break;
    }
}

static void draw_window(const window_t *w, int focused) {
    int x = w->x, y = w->y, ww = w->w, hh = w->h;

    /* soft drop shadow: feathered, offset layers (down-right) — darker near the
     * window edge and fading out, for real depth. The layers overlap and darken
     * multiplicatively, so the band nearest the window is darkest and the outer
     * fringe is faint, approximating a Gaussian-ish soft shadow. */
    darken(x + 11, y + 13, ww, hh, 90);
    darken(x + 8,  y + 10, ww, hh, 86);
    darken(x + 5,  y + 7,  ww, hh, 82);
    darken(x + 2,  y + 4,  ww, hh, 78);

    /* body */
    fb_fill_rect(x, y, ww, hh, w->body);
    fb_set_clip(x, y, x + ww, y + hh);    /* bound this window's content to its rect — a long line / future layout can't bleed onto a neighbour or the taskbar (the per-kind min-sizes above are belt-and-braces) */
    draw_content(w, focused);
    fb_reset_clip();

    /* gradient title bar (brighter when focused) with a top sheen line */
    uint32_t t0 = focused ? 0x6BA8FF : 0x6E7686;
    uint32_t t1 = focused ? 0x1F56C6 : 0x3A4150;
    vgrad(x, y, ww, TITLEBAR_H, t0, t1);
    fb_fill_rect(x, y, ww, 1, lerp(t0, 0xFFFFFF, 2, 3));   /* bright top sheen */
    const char *titletext = (w->kind == KIND_BROWSER && w->app) ? browser_title((browser_t *)w->app) : w->title;   /* browser windows show the page <title> / document.title */
    draw_icon(w->kind, x + 8, y + (TITLEBAR_H - ICON_SZ) / 2);                /* per-kind icon */
    draw_text(x + 28, y + (TITLEBAR_H - font_height) / 2 + 1, titletext, 0xFFFFFF);

    /* close button: a rounded red chip with a top sheen; calmer when unfocused */
    int cbx = x + ww - 21, cby = y + 6;
    uint32_t cc0 = focused ? 0xFF7B7B : 0xCF6A6A, cc1 = focused ? 0xD42B2B : 0x9E3232;
    vgrad(cbx, cby, 14, 14, cc0, cc1);
    fb_fill_rect(cbx, cby, 14, 1, lerp(cc0, 0xFFFFFF, 1, 2));
    round_chrome(cbx, cby, 14, 14, t0, t1, y, TITLEBAR_H);
    draw_text(cbx + 3, cby - 1, "x", 0xFFFFFF);

    /* resize grip hatching */
    for (int i = 4; i < GRIP; i += 4)
        fb_fill_rect(x + ww - i, y + hh - 3, i - 2, 2, 0x9098A4);

    /* beveled border: light inner highlight, dark outer edge + rounded corners */
    box(x, y, ww, hh, 0x0A0E16);
    fb_fill_rect(x + 1, y + TITLEBAR_H, ww - 2, 1, focused ? 0x1B3F86 : 0x2A2F39);
    for (int j = 0; j < CORNER_R; j++) {              /* round the 4 corners */
        int in = corner[j];
        for (int i = 0; i < in; i++) {
            fb_pixel(x + i, y + j, wallpaper_at(x + i, y + j));
            fb_pixel(x + ww - 1 - i, y + j, wallpaper_at(x + ww - 1 - i, y + j));
            fb_pixel(x + i, y + hh - 1 - j, wallpaper_at(x + i, y + hh - 1 - j));
            fb_pixel(x + ww - 1 - i, y + hh - 1 - j, wallpaper_at(x + ww - 1 - i, y + hh - 1 - j));
        }
    }
}

/* Decode image file `name` into a freshly-allocated screen-sized 0x00RRGGBB
 * bitmap, scaling the decoded image (nearest-neighbour) to screen_w*screen_h so
 * ANY image — not just a screen-exact one — works as wallpaper. Returns the
 * buffer (caller owns it), or NULL on any failure (missing / undecodable / OOM).
 * All scratch is freed before returning; no partial allocation is leaked.
 *
 * decode_image() (browser.c) dispatches on the magic bytes to png/bmp/jpeg/gif/
 * svg_decode and returns the image at its NATIVE w*h, so any format the OS reads
 * works here — including a source larger than the screen (capped 2048x2048/1M
 * px by the decoder; oversize -> NULL, leaving the caller's wallpaper untouched). */
static uint32_t *decode_wallpaper(const char *name) {
    long npix = (long)screen_w * screen_h;
    uint8_t *file = kmalloc(512 * 1024);
    if (!file) return NULL;
    long n = vfs_read(name, file, 512 * 1024);
    if (n <= 0) { kfree(file); return NULL; }
    int w = 0, h = 0;
    uint8_t *rgba = decode_image(file, (int)n, &w, &h);     /* native-size RGBA (we own it) */
    kfree(file);
    if (!rgba || w <= 0 || h <= 0) { if (rgba) kfree(rgba); return NULL; }
    uint32_t *bmp = kmalloc((size_t)npix * 4);
    if (!bmp) { kfree(rgba); return NULL; }
    for (int dy = 0; dy < screen_h; dy++) {                 /* nearest-neighbour scale to screen */
        int sy = (int)((long)dy * h / screen_h);            /* src_y = dst_y * src_h / screen_h */
        for (int dx = 0; dx < screen_w; dx++) {
            int sx = (int)((long)dx * w / screen_w);        /* src_x = dst_x * src_w / screen_w */
            const uint8_t *p = &rgba[((long)sy * w + sx) * 4];   /* RGBA bytes -> 0x00RRGGBB */
            bmp[(long)dy * screen_w + dx] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    kfree(rgba);
    return bmp;
}

/* Procedural desktop background (visual refresh): a deep corner-to-corner blue
 * gradient with a soft radial vignette + a gentle glow above center. Integer-only
 * (the kernel has no FPU). Looks clean + modern AND needs no image on disk, so the
 * desktop looks good even on bare metal where WALL.PNG may be absent. */
static void make_wallpaper(uint32_t *buf, int w, int h) {
    uint32_t c0 = 0x12315C, c1 = 0x3A82C4;        /* deep blue -> vibrant azure (diagonal) */
    int cx = w / 2, gy = (h * 40) / 100;          /* glow center, a touch above middle */
    long far = (long)cx * cx + (long)(h - gy) * (h - gy);
    if (far < 1) far = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t base = lerp(c0, c1, x + y, (w + h) - 2);     /* diagonal gradient */
            long dx = x - cx, dy = y - gy, d2 = dx * dx + dy * dy;
            int t = (int)(d2 * 100 / far); if (t > 100) t = 100;  /* 0 = glow center .. 100 = far corner */
            int fac = 118 - (118 - 78) * t / 100;                 /* +18% glow at center -> -22% vignette at the corners */
            int r = ((base >> 16) & 0xFF) * fac / 100; if (r > 255) r = 255;
            int g = ((base >>  8) & 0xFF) * fac / 100; if (g > 255) g = 255;
            int b = ( base        & 0xFF) * fac / 100; if (b > 255) b = 255;
            buf[(size_t)y * w + x] = (uint32_t)(r << 16 | g << 8 | b);
        }
}

/* The boot desktop background. A clean PROCEDURAL gradient is the default (works
 * with no image on disk, incl. bare metal); a user can still load a custom image
 * at runtime with the `wallpaper` shell builtin (desktop_set_wallpaper). */
static void load_wallpaper(void) {
    wallpaper_bmp = kmalloc((size_t)screen_w * screen_h * 4);
    if (wallpaper_bmp) make_wallpaper(wallpaper_bmp, screen_w, screen_h);
}

/* Change the desktop wallpaper at runtime (the `wallpaper` shell builtin, via
 * SYS_setwall). Decode-into-new-then-swap: a failed load NEVER disturbs the live
 * wallpaper. The swap is a single pointer store; this runs inside a syscall with
 * interrupts disabled (IF=0), so it is atomic w.r.t. the desktop render task —
 * the renderer can never observe a half-updated wallpaper_bmp, and freeing the
 * old buffer can't race a read of it. Returns 0 on success, -1 on any failure
 * (the current wallpaper is kept). */
int desktop_set_wallpaper(const char *name) {
    uint32_t *next = decode_wallpaper(name);
    if (!next) return -1;                                  /* keep the current wallpaper */
    uint32_t *old = wallpaper_bmp;
    wallpaper_bmp = next;                                  /* atomic swap (IF=0) */
    kfree(old);                                            /* free(NULL) is a no-op (boot gradient) */
    wallpaper_repaint = 1;                                 /* nudge the WM loop to redraw promptly */
    return 0;
}

/* Row count + width of the currently-open context menu (per ctx_kind). Shared by
 * the open/render/hit-test so the popup is always sized for whichever kind is up. */
static int ctx_nrows(void) { return ctx_kind == 1 ? CTX_DESK_ROWS : CTX_ROWS; }
static int ctx_w(void)     { return ctx_kind == 1 ? CTX_DESK_W    : CTX_W;     }

/* Context-menu hit-test: which row (0..ctx_nrows()-1) is at cursor (px,py), or -1
 * if outside the popup. Shared by the renderer's hover highlight and the click
 * handler so they always agree on the row boundaries (cf. clk_pill_w). */
static int ctx_row_at(int px, int py) {
    if (px < ctx_x || px >= ctx_x + ctx_w()) return -1;
    int r = (py - (ctx_y + 2)) / CTX_ROW_H;
    return (r >= 0 && r < ctx_nrows()) ? r : -1;
}

/* Render the whole scene (wallpaper, windows, taskbar — but NOT the cursor)
 * into the cached scene buffer. This is the expensive part, so we only do it
 * when the scene actually changes; plain cursor moves reuse the cache. */
static void render_scene(void) {
    fb_set_target(scenebuf);
    wp_h = screen_h;
    if (wallpaper_bmp)                                      /* image from disk */
        memcpy(scenebuf, wallpaper_bmp, (size_t)screen_w * screen_h * 4);
    else
        vgrad(0, 0, screen_w, screen_h, WP_TOP, WP_BOT);    /* fallback: gradient */

    for (int i = 0; i < win_count; i++)
        if (!windows[i].minimized)                          /* minimized = hidden to its chip */
            draw_window(&windows[i], i == win_count - 1);

    /* taskbar: gradient panel with a bright accent line on top */
    int ty = screen_h - TASKBAR_H;
    vgrad(0, ty, screen_w, TASKBAR_H, 0x222C3C, 0x0D1119);
    fb_fill_rect(0, ty, screen_w, 2, 0x4A90E2);

    /* Apps button: gradient + bevel */
    uint32_t a0 = menu_open ? 0x6BA6F5 : 0x4A90E2, a1 = menu_open ? 0x3A78D8 : 0x2C66D6;
    vgrad(start_x, start_y, start_w, start_h, a0, a1);
    fb_fill_rect(start_x, start_y, start_w, 1, lerp(a0, 0xFFFFFF, 1, 2));
    box(start_x, start_y, start_w, start_h, 0x18345E);
    round_chrome(start_x, start_y, start_w, start_h, 0x222C3C, 0x0D1119, ty, TASKBAR_H);
    int gi_x = start_x + 9, gi_y = start_y + (start_h - 10) / 2;  /* 2x2 "apps" grid icon */
    fb_fill_rect(gi_x,     gi_y,     4, 4, 0xFFFFFF);
    fb_fill_rect(gi_x + 6, gi_y,     4, 4, 0xFFFFFF);
    fb_fill_rect(gi_x,     gi_y + 6, 4, 4, 0xFFFFFF);
    fb_fill_rect(gi_x + 6, gi_y + 6, 4, 4, 0xFFFFFF);
    draw_text(start_x + 26, start_y + 4, "Apps", 0xFFFFFF);

    /* real-time clock (RTC) in a recessed pill on the right: date + time, so the
     * day is visible at a glance without opening the Calendar app. */
    struct rtc_time tm; rtc_now(&tm);
    char clk[24]; int q = 0;                                  /* "YYYY-MM-DD  HH:MM:SS" */
    clk[q++]='0'+(tm.year/1000)%10; clk[q++]='0'+(tm.year/100)%10;
    clk[q++]='0'+(tm.year/10)%10;   clk[q++]='0'+tm.year%10;
    clk[q++]='-'; u2((uint64_t)tm.month, clk+q); q+=2;
    clk[q++]='-'; u2((uint64_t)tm.day,   clk+q); q+=2;
    clk[q++]=' '; clk[q++]=' ';
    u2((uint64_t)tm.hour, clk+q); q+=2; clk[q++]=':';
    u2((uint64_t)tm.min,  clk+q); q+=2; clk[q++]=':';
    u2((uint64_t)tm.sec,  clk+q); q+=2; clk[q]=0;
    int clkw = clk_pill_w(), clkx = screen_w - clkw - 8;

    /* one chip per open window (the focused one — topmost — is highlighted) */
    for (int i = 0; i < win_count; i++) {
        int cx = TB_CHIPX0 + i * (TB_CHIPW + TB_CHIPGAP);
        if (cx + TB_CHIPW > clkx - 8) break;                  /* out of room */
        int foc = (i == win_count - 1);
        int mini = windows[i].minimized;                      /* hidden: dim its chip */
        vgrad(cx, start_y, TB_CHIPW, start_h, foc ? 0x3A78D8 : (mini ? 0x1B222E : 0x2A3344),
              foc ? 0x2C66D6 : 0x161D29);
        box(cx, start_y, TB_CHIPW, start_h, foc ? 0x4A90E2 : 0x33415A);
        round_chrome(cx, start_y, TB_CHIPW, start_h, 0x222C3C, 0x0D1119, ty, TASKBAR_H);
        draw_icon(windows[i].kind, cx + 6, start_y + (start_h - ICON_SZ) / 2);   /* per-kind icon */
        char t[18]; int n = 0; const char *s = windows[i].title;
        while (s && s[n] && n < 12) { t[n] = s[n]; n++; } t[n] = 0;
        draw_text(cx + 26, start_y + 4, t, foc ? 0xFFFFFF : (mini ? 0x6B7689 : 0xAEB8C8));
    }
    fb_fill_rect(clkx, start_y, clkw, start_h, 0x10151E);
    box(clkx, start_y, clkw, start_h, 0x33415A);
    round_chrome(clkx, start_y, clkw, start_h, 0x222C3C, 0x0D1119, ty, TASKBAR_H);
    draw_text(clkx + 14, start_y + 4, clk, 0x9FC0F0);

    if (menu_open) {
        int mh = MENU_PERCOL * MENU_ITEM_H + 4, mw = MENU_COLS * MENU_W, my0 = ty - mh;
        vgrad(start_x, my0, mw, mh, 0x262636, 0x14141D);             /* gradient panel */
        fb_fill_rect(start_x, my0, mw, 2, 0x4A90E2);                 /* bright top accent */
        for (int c = 1; c < MENU_COLS; c++)                          /* faint column rules */
            fb_fill_rect(start_x + c * MENU_W, my0 + 4, 1, mh - 8, 0x30303F);
        box(start_x, my0, mw, mh, 0x2D6CDF);
        for (int i = 0; i < MENU_N; i++) {
            int col = i / MENU_PERCOL, row = i % MENU_PERCOL;
            int ix = start_x + col * MENU_W, iy = my0 + 4 + row * MENU_ITEM_H;
            if (i == menu_sel) {                                     /* keyboard highlight: gradient bar + left accent */
                vgrad(ix + 2, iy, MENU_W - 4, MENU_ITEM_H, 0x3A78D8, 0x2C66D6);
                fb_fill_rect(ix + 2, iy, 2, MENU_ITEM_H, 0x8FC0FF);
            }
            draw_text(ix + 12, iy + 2, menu[i].label, i == menu_sel ? 0xFFFFFF : 0xD0D8F0);
        }
    }

    /* Right-click context menu: a small popup at the click point, drawn on top of
     * the windows (like the Apps menu). Two kinds share this block (ctx_kind): the
     * window title-bar menu (ctx_win names the topmost window, so a closed/reordered
     * window can't leave a stale row — render is skipped if it's no longer valid)
     * and the desktop-background menu (no window dependency). The row under the
     * cursor is highlighted (same colours as the Apps menu's keyboard highlight). */
    if (ctx_open && (ctx_kind == 1 || (ctx_win >= 0 && ctx_win < win_count))) {
        static const char *desk_rows[CTX_DESK_ROWS] = {
            "Show Desktop", "Show All Windows", "Change Wallpaper",
        };
        const char *win_rows[CTX_ROWS] = {
            windows[ctx_kind == 0 ? ctx_win : 0].maximized ? "Restore" : "Maximize",
            "Minimize", "Snap Left", "Snap Right", "Close",
        };
        const char **rows = (ctx_kind == 1) ? desk_rows : win_rows;
        int nr = ctx_nrows(), cw = ctx_w(), ch = nr * CTX_ROW_H + 4;
        vgrad(ctx_x, ctx_y, cw, ch, 0x262636, 0x14141D);     /* gradient panel (matches the Apps menu) */
        fb_fill_rect(ctx_x, ctx_y, cw, 2, 0x4A90E2);         /* bright top accent */
        box(ctx_x, ctx_y, cw, ch, 0x2D6CDF);
        int sel = ctx_row_at(mouse_x(), mouse_y());          /* row under the cursor (-1 = none) */
        for (int i = 0; i < nr; i++) {
            int iy = ctx_y + 2 + i * CTX_ROW_H;
            if (i == sel) {                                      /* hover highlight: gradient bar + left accent */
                vgrad(ctx_x + 2, iy, cw - 4, CTX_ROW_H, 0x3A78D8, 0x2C66D6);
                fb_fill_rect(ctx_x + 2, iy, 2, CTX_ROW_H, 0x8FC0FF);
            }
            draw_text(ctx_x + 12, iy + 3, rows[i], i == sel ? 0xFFFFFF : 0xD0D8F0);
        }
    }

    /* F1 help overlay: a centered panel listing every keyboard shortcut + the
     * mouse gestures. Drawn last so it sits on top of everything. */
    if (help_open) {
        static const char *H[] = {
            "F1    this help",        "F2    switch window",
            "F3    minimize",         "F4    maximize / restore",
            "F5    snap left",        "F6    snap right",
            "F7    window switcher",  "F8    close window",
            "F9    Apps menu",
            "F12   screenshot to disk",
            "",
            "Mouse: drag the title bar to move a window",
            "(double-click it, or drag to the top edge, to",
            "maximize; drag to a side edge to tile that half),",
            "drag the bottom-right corner to resize,",
            "click [x] to close, click a taskbar chip to",
            "raise (or restore) that window.",
            "Right-click a title bar for a menu (maximize,",
            "minimize, snap left/right, close).",
            "Right-click the desktop for a menu (show",
            "desktop, show all windows, change wallpaper).",
            "",
            "Wheel scrolls the window under the cursor.",
            "In a terminal: drag (or double-click a word)",
            "to select + copy; middle-click pastes.",
            "In the browser: right-click a link to copy",
            "its URL; middle-click pastes into the bar.",
            "In Files: Enter opens, d deletes (press d/y",
            "again to confirm), r renames, n makes a new",
            "folder, w sets an image as wallpaper.",
            "",
            "Press Esc or F1 to close this help.",
        };
        int n = (int)(sizeof(H) / sizeof(H[0]));
        int pw = 380, ph = n * 18 + 40;
        int px = (screen_w - pw) / 2, py = (screen_h - TASKBAR_H - ph) / 2;
        vgrad(px, py, pw, ph, 0x1C1C28, 0x12121A);           /* subtly graded panel body */
        box(px, py, pw, ph, 0x2D6CDF);
        vgrad(px, py, pw, 26, 0x3A78D8, 0x2C66D6);            /* title bar */
        fb_fill_rect(px, py, pw, 1, lerp(0x3A78D8, 0xFFFFFF, 1, 2));
        draw_text(px + 12, py + (26 - font_height) / 2 + 1, "Keyboard Shortcuts", 0xFFFFFF);
        for (int i = 0; i < n; i++)
            draw_text(px + 16, py + 34 + i * 18, H[i], 0xCFD8EC);
    }

    /* F7 window switcher: a centered panel listing every window (incl. minimized,
     * marked "(min)") with the highlighted one selected; Enter raises+restores it.
     * Drawn last so it sits on top of everything. */
    if (sw_open) {
        if (sw_sel >= win_count) sw_sel = win_count - 1;   /* a window closed while open: clamp */
        if (sw_sel < 0) sw_sel = 0;
        int rows = win_count > 0 ? win_count : 1;
        int pw = 320, ph = rows * MENU_ITEM_H + 40;
        int px = (screen_w - pw) / 2, py = (screen_h - TASKBAR_H - ph) / 2;
        vgrad(px, py, pw, ph, 0x1C1C28, 0x12121A);           /* subtly graded panel body */
        box(px, py, pw, ph, 0x2D6CDF);
        vgrad(px, py, pw, 26, 0x3A78D8, 0x2C66D6);            /* title bar */
        fb_fill_rect(px, py, pw, 1, lerp(0x3A78D8, 0xFFFFFF, 1, 2));
        draw_text(px + 12, py + (26 - font_height) / 2 + 1, "Windows", 0xFFFFFF);
        if (win_count == 0) {
            draw_text(px + 16, py + 34, "(no windows)", 0x9FB0CC);
        } else for (int i = 0; i < win_count; i++) {
            int iy = py + 30 + i * MENU_ITEM_H;
            if (i == sw_sel) {                                /* keyboard highlight: gradient bar + left accent */
                vgrad(px + 4, iy, pw - 8, MENU_ITEM_H, 0x3A78D8, 0x2C66D6);
                fb_fill_rect(px + 4, iy, 2, MENU_ITEM_H, 0x8FC0FF);
            }
            char t[40]; int n = 0; const char *s = windows[i].title;
            while (s && s[n] && n < 28) { t[n] = s[n]; n++; }
            if (windows[i].minimized) {                       /* mark hidden windows */
                const char *m = " (min)";
                for (int j = 0; m[j] && n < (int)sizeof(t) - 1; j++) t[n++] = m[j];
            }
            t[n] = 0;
            draw_text(px + 16, iy + 4, t, i == sw_sel ? 0xFFFFFF : 0xD0D8F0);
        }
    }
}

/* Last position at which we painted the cursor into the back buffer, so a
 * cursor-only update can repair exactly that rectangle. -1 = none painted yet. */
static int cur_px = -1, cur_py = -1;

/* Copy a clipped rectangle from the cached scene back into the back buffer
 * (erasing whatever — e.g. the old cursor — was drawn over the scene there). */
static void restore_scene_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        memcpy(backbuffer + (size_t)(y + j) * screen_w + x,
               scenebuf   + (size_t)(y + j) * screen_w + x, (size_t)w * 4);
}

/* Copy the cached scene to the back buffer, draw the cursor on top, and blit
 * the whole screen. Used whenever the scene itself changed. */
static void present_frame(void) {
    int nx = mouse_x(), ny = mouse_y();              /* one consistent snapshot */
    memcpy(backbuffer, scenebuf, (size_t)screen_w * screen_h * 4);
    fb_set_target(backbuffer);
    mouse_paint_at(nx, ny);
    fb_present();
    cur_px = nx; cur_py = ny;
}

/* The cursor moved but the scene didn't: repair the old cursor rectangle from
 * the scene cache, draw the cursor at its new spot, and flush ONLY those two
 * small rectangles — instead of memcpy-ing and blitting the whole ~3 MB frame.
 * (M52 left this as a future optimisation; this is it.) */
static void present_cursor(void) {
    if (cur_px < 0) { present_frame(); return; }    /* nothing painted yet */
    int cw = mouse_cursor_w(), ch = mouse_cursor_h();
    int nx = mouse_x(), ny = mouse_y();             /* sample position ONCE */
    fb_set_target(backbuffer);
    restore_scene_rect(cur_px, cur_py, cw, ch);     /* erase old cursor */
    mouse_paint_at(nx, ny);                         /* draw cursor at the snapshot */
    fb_present_rect(cur_px, cur_py, cw, ch);        /* flush repaired old rect */
    fb_present_rect(nx, ny, cw, ch);                /* flush new cursor rect */
    cur_px = nx; cur_py = ny;
}

static void raise_window(int idx) {
    window_t tmp = windows[idx];
    for (int i = idx; i < win_count - 1; i++) windows[i] = windows[i + 1];
    windows[win_count - 1] = tmp;
    windows[win_count - 1].minimized = 0;       /* raising a window also restores it */
}
/* F3: send a window to the bottom of the z-order (used when minimizing, so the
 * next window below becomes focused). The reverse of raise_window. */
static void sink_window(int idx) {
    window_t tmp = windows[idx];
    for (int i = idx; i > 0; i--) windows[i] = windows[i - 1];
    windows[0] = tmp;
}
/* F4: fill the screen (above the taskbar), or restore the saved geometry. */
static void toggle_maximize(int idx) {
    window_t *w = &windows[idx];
    if (w->maximized) {
        w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
        w->maximized = 0;
    } else {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->x = 0; w->y = 0; w->w = screen_w; w->h = screen_h - TASKBAR_H;
        w->maximized = 1;
    }
}
/* F5/F6: snap the window to the left/right half of the screen (tiling). Saves
 * the original geometry the same way maximize does, so F4 / a drag restores. */
static void snap_window(int idx, int rightside) {
    window_t *w = &windows[idx];
    if (!w->maximized) { w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h; }
    int half = screen_w / 2;
    w->x = rightside ? half : 0;
    w->y = 0; w->w = half; w->h = screen_h - TASKBAR_H;
    w->maximized = 1;
}
static void remove_window(int idx) {
    if (windows[idx].kind == KIND_BROWSER && windows[idx].app)
        browser_destroy((browser_t *)windows[idx].app);   /* free its buffers */
    for (int i = idx; i < win_count - 1; i++) windows[i] = windows[i + 1];
    win_count--;
}

/* Run the right-click context-menu row `row` on window `idx`. Each branch wraps
 * the exact helper the matching F-key uses, so behaviour is identical. Returns 1
 * if it reordered/removed the window array (the caller must then drop any active
 * mouse gesture, as the F3/F8 paths do); 0 if geometry-only. `idx` must be valid. */
static int ctx_action(int idx, int row) {
    switch (row) {
        case 0: toggle_maximize(idx); return 0;                  /* Maximize / Restore (F4) */
        case 1: {                                                /* Minimize (F3) */
            int vis = 0;                                         /* never hide the LAST visible window */
            for (int i = 0; i < win_count; i++) if (!windows[i].minimized) vis++;
            if (vis > 1) { windows[idx].minimized = 1; sink_window(idx); return 1; }
            return 0;
        }
        case 2: snap_window(idx, 0); return 0;                   /* Snap Left (F5) */
        case 3: snap_window(idx, 1); return 0;                   /* Snap Right (F6) */
        case 4:                                                  /* Close (F8) */
            if (windows[idx].kind == KIND_APP && windows[idx].app)
                app_request_kill((app_t *)windows[idx].app);     /* app self-exits; reap loop drops the window */
            else
                remove_window(idx);                              /* browser/files/etc: drop immediately */
            return 1;
    }
    return 0;
}

static int files_is_image(const char *name, int len);   /* defined below (shared with the Files 'w' key) */

/* Run the desktop-background context-menu row `row`. Returns 1 if it reordered the
 * window array (so the caller drops any active mouse gesture), 0 otherwise.
 *   0 Show Desktop      minimize EVERY window (no last-window guard, unlike F3)
 *   1 Show All Windows  restore every minimized window
 *   2 Change Wallpaper  cycle to the next image file on disk */
static int ctx_desktop_action(int row) {
    switch (row) {
        case 0:                                                  /* Show Desktop: minimize EVERY window */
            for (int i = 0; i < win_count; i++)                  /* a flag-only pass (no array mutation, */
                windows[i].minimized = 1;                        /* so none is skipped); z-order among */
            return 0;                                            /* hidden windows is irrelevant, so no sink */
        case 1:                                                  /* Show All Windows: restore every minimized one */
            for (int i = 0; i < win_count; i++)                  /* flag-only pass too: raise_window would */
                windows[i].minimized = 0;                        /* shift the array mid-iteration and skip */
            return 0;                                            /* one; z-order is preserved (all visible) */
        case 2: {                                                /* Change Wallpaper: cycle to the next image on disk */
            static int wp_idx;
            static vfs_dirent e[256]; int n = vfs_list(e, 256);  /* static (BSS), safe single-threaded (cf. files_key) */
            if (n <= 0) return 0;
            for (int step = 0; step < n; step++) {               /* try each candidate from wp_idx onward; */
                int i = (wp_idx + step) % n;                     /* a non-image / decode failure is skipped */
                const char *name = e[i].name;
                int len = 0; while (name[len]) len++;
                if (len > 0 && files_is_image(name, len) && desktop_set_wallpaper(name) == 0) {
                    wp_idx = (i + 1) % n;                        /* next call starts past the one we just set */
                    return 0;
                }
            }
            return 0;                                            /* no usable image: no-op */
        }
    }
    return 0;
}

/* Give a freshly-spawned app a window (called by the WM as it drains the
 * spawn queue, so apps launched from anywhere get a window here). */
static void make_app_window(app_t *a) {
    if (win_count >= MAX_WINDOWS) return;
    spawn_n++;
    int x = 150 + (spawn_n % 6) * 26, y = 60 + (spawn_n % 6) * 26;
    windows[win_count++] = (window_t){ x, y,
        app_cols()*font_width + 14, app_rows()*font_height + TITLEBAR_H + 14,
        0x0A0A0A, app_title(a), KIND_APP, a, 0,0,0,0,0,0,0, 0,{0},0, 0 };  /* maximized,sx,sy,sw,sh,fsel,fconfirm, editing,editbuf,editlen, minimized */
}

/* Open a browser window at `url` (NULL -> its default). */
static void spawn_browser(const char *url) {
    if (win_count >= MAX_WINDOWS) return;
    spawn_n++;
    int x = 150 + (spawn_n % 6) * 26, y = 60 + (spawn_n % 6) * 26;
    windows[win_count++] = (window_t){ x, y, 620, 460, 0xFFFFFF, "Browser",
                                       KIND_BROWSER, browser_create(url), 0,0,0,0,0,0,0, 0,{0},0, 0 };
}

/* Is `name` a plain-text / source file we'd rather edit than view? (case-
 * insensitive extension match; FAT names are upper-case). */
static int files_editable(const char *name, int len) {
    int dot = -1; for (int i = 0; i < len; i++) if (name[i] == '.') dot = i;
    if (dot < 0) return 0;
    static const char *exts[] = { "TXT","MD","C","H","SH","LOG","CFG","INI","JS","ASM","JSON", 0 };
    for (int i = 0; exts[i]; i++) {
        const char *a = name + dot + 1, *b = exts[i]; int eq = 1;
        while (*a && *b) { char ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a; if (ca != *b) { eq = 0; break; } a++; b++; }
        if (eq && !*a && !*b) return 1;
    }
    return 0;
}

/* Is `name` an image we can set as the wallpaper? (case-insensitive extension
 * match — same formats decode_image() handles: PNG/BMP/JPG/JPEG/GIF/SVG.) */
static int files_is_image(const char *name, int len) {
    int dot = -1; for (int i = 0; i < len; i++) if (name[i] == '.') dot = i;
    if (dot < 0) return 0;
    static const char *exts[] = { "PNG","BMP","JPG","JPEG","GIF","SVG", 0 };
    for (int i = 0; exts[i]; i++) {
        const char *a = name + dot + 1, *b = exts[i]; int eq = 1;
        while (*a && *b) { char ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a; if (ca != *b) { eq = 0; break; } a++; b++; }
        if (eq && !*a && !*b) return 1;
    }
    return 0;
}

/* Keyboard handling for the Files window. up/down move the selection; Enter
 * opens the highlighted file (text/source -> editor, else a browser window).
 * 'd'/Delete arms a two-key delete confirm (a second d/y commits it; ANY other
 * key cancels it — so a stray 'd' is harmless); 'w' sets an image as the
 * wallpaper. The dirent list is re-read each call, so it refreshes for free
 * after a delete. */
static void files_key(window_t *w, int k) {
    static vfs_dirent e[256]; int n = vfs_list(e, 256);   /* match the render cap; static (BSS), safe single-threaded (M421) */

    if (w->editing) {                                         /* a rename / new-folder text-input is open */
        if (k == 27) { w->editing = 0; return; }               /* Esc cancels: leave the name untouched */
        if (k == '\n' || k == '\r') {                          /* Enter commits */
            w->editbuf[w->editlen] = 0;
            if (w->editlen > 0) {                              /* empty input is a no-op (vfs_rename/mkdir would reject it anyway) */
                if (w->editing == 1) {                         /* rename the selected entry */
                    if (n > 0 && w->fsel >= 0 && w->fsel < n) {
                        char old[64]; int p = 0;
                        for (int j = 0; e[w->fsel].name[j] && p < (int)sizeof(old) - 1; j++) old[p++] = e[w->fsel].name[j];
                        if (p > 0 && old[p-1] == '/') p--;     /* the listing marks dirs with a trailing '/' */
                        old[p] = 0;
                        vfs_rename(old, w->editbuf);           /* -1 (bad name / clobber / missing) leaves the disk unchanged */
                    }
                } else {                                       /* editing == 2: make a new folder */
                    vfs_mkdir(w->editbuf);                     /* -1 on a bad name / existing entry: no-op */
                }
            }
            w->editing = 0;
            n = vfs_list(e, 256);                              /* re-list so the new/renamed entry shows + the clamp is correct */
            if (w->fsel >= n) w->fsel = n - 1;
            if (w->fsel < 0)  w->fsel = 0;
            return;
        }
        if (k == 8 || k == 0x7F) {                             /* Backspace / Delete: drop the last char */
            if (w->editlen > 0) w->editbuf[--w->editlen] = 0;
            return;
        }
        if (k >= ' ' && k < 0x7F && w->editlen < 12) {         /* printable: append (8.3 = max 12 chars), upper-cased like the FS stores */
            char c = (k >= 'a' && k <= 'z') ? (char)(k - 32) : (char)k;
            w->editbuf[w->editlen++] = c;
            w->editbuf[w->editlen] = 0;
        }
        return;                                                /* swallow every key while editing — don't fall through to nav */
    }

    if (n <= 0) { w->fconfirm = 0; return; }
    if (w->fsel >= n) w->fsel = n - 1;
    if (w->fsel < 0)  w->fsel = 0;

    if (w->fconfirm) {                                         /* a delete is armed */
        if (k == 'd' || k == 'y' || k == 0x7F) {               /* second d/y (or Delete) -> commit */
            char buf[64]; int p = 0;
            for (int j = 0; e[w->fsel].name[j] && p < (int)sizeof(buf) - 1; j++) buf[p++] = e[w->fsel].name[j];
            if (p > 0 && buf[p-1] == '/') p--;                 /* strip the listing's trailing '/' on dirs */
            buf[p] = 0;
            vfs_remove(buf);                                   /* deletes a file or empty dir; refuses a non-empty dir (no crash) */
            w->fconfirm = 0;
            n = vfs_list(e, 256);                              /* re-list so the clamp uses the post-delete count */
            if (w->fsel >= n) w->fsel = n - 1;
            if (w->fsel < 0)  w->fsel = 0;
        } else {                                               /* ANY other key cancels; the key is otherwise ignored */
            w->fconfirm = 0;
        }
        return;
    }

    if (k == 0x11)       { if (w->fsel > 0)     w->fsel--; }   /* up   */
    else if (k == 0x12)  { if (w->fsel < n - 1) w->fsel++; }   /* down */
    else if (k == 8)     { if (vfs_chdir("..") == 0) { n = vfs_list(e, 256); w->fsel = 0; } }  /* Backspace: up one directory */
    else if (k == 'd' || k == 0x7F) {                          /* arm the delete confirm (render shows the prompt) */
        w->fconfirm = 1;
    }
    else if (k == 'w') {                                       /* set an image file as the desktop wallpaper */
        const char *name = e[w->fsel].name;
        int len = 0; while (name[len]) len++;
        if (len > 0 && files_is_image(name, len))
            desktop_set_wallpaper(name);                       /* visible bg change is the feedback; a decode failure is a no-op */
    }
    else if (k == 'r') {                                       /* rename: open a text-input pre-filled with the selected name */
        int p = 0;
        for (int j = 0; e[w->fsel].name[j] && p < 12; j++) w->editbuf[p++] = e[w->fsel].name[j];
        if (p > 0 && w->editbuf[p-1] == '/') p--;              /* drop the dir-marker '/' so the buffer holds just the name */
        w->editbuf[p] = 0; w->editlen = p;
        w->editing = 1;
    }
    else if (k == 'n') {                                       /* new folder: open an empty text-input */
        w->editbuf[0] = 0; w->editlen = 0;
        w->editing = 2;
    }
    else if (k == '\n' || k == '\r') {
        const char *name = e[w->fsel].name;
        int len = 0; while (name[len]) len++;
        if (len > 0 && name[len-1] == '/') {                   /* a directory: navigate into it */
            char d[64]; int p = 0;
            for (int j = 0; j < len - 1 && p < (int)sizeof(d) - 1; j++) d[p++] = name[j];  /* strip trailing '/' */
            d[p] = 0;
            if (vfs_chdir(d) == 0) { n = vfs_list(e, 256); w->fsel = 0; }   /* enter folder + re-list */
        } else if (len > 0) {                                  /* a file: open it */
            if (files_editable(name, len)) {
                app_spawn_named_arg("editor", name);           /* edit text/source files */
            } else {
                char url[32]; int p = 0; const char *pre = "file:";
                while (*pre) url[p++] = *pre++;
                for (int j = 0; name[j] && p < (int)sizeof(url) - 1; j++) url[p++] = name[j];
                url[p] = 0;
                spawn_browser(url);                            /* view everything else */
            }
        }
    }
}

/* A click in the Files window body: pick the file row under the cursor, select
 * it, and open it (the mouse equivalent of arrowing to it and pressing Enter).
 * `my` is the screen y of the click; the row math mirrors the KIND_FILES render. */
static void files_click(window_t *w, int my) {
    static vfs_dirent e[256]; int n = vfs_list(e, 256);
    if (n <= 0) return;
    int by = w->y + TITLEBAR_H + 8;                    /* body content origin (matches draw) */
    int rows = (w->h - TITLEBAR_H - 30) / 18; if (rows < 1) rows = 1;
    int top = (w->fsel >= rows) ? w->fsel - rows + 1 : 0;   /* same scroll as the render */
    if (my < by + 22) return;                          /* clicked the header line, not a file */
    int row = top + (my - (by + 22)) / 18;
    if (row < 0 || row >= n) return;                   /* clicked below the last file */
    w->fsel = row;
    files_key(w, '\n');                                /* select + open via the shared Enter path */
}

static void spawn_app(int kind, const char *prog) {
    if (kind == KIND_APP)     { app_spawn_named(prog); return; }  /* WM drains it */
    if (kind == KIND_BROWSER) { spawn_browser(0); return; }
    if (kind == KIND_POWEROFF) { acpi_poweroff(); return; }       /* ACPI S5 — the machine powers off */
    if (kind == KIND_REBOOT)   { acpi_reboot();   return; }       /* ACPI reset (8042 fallback) */
    if (win_count >= MAX_WINDOWS) return;
    spawn_n++;
    int x = 150 + (spawn_n % 6) * 26, y = 60 + (spawn_n % 6) * 26;
    window_t w = { 0 };
    w.x = x; w.y = y; w.kind = kind;
    switch (kind) {
    case KIND_FILES:   w.w=500; w.h=200; w.body=0xE8ECF4; w.title="Files";   break;  /* wide enough for the d-delete / w-wallpaper hint + confirm prompt */
    case KIND_WELCOME: w.w=360; w.h=290; w.body=0xF0F0F0; w.title="Welcome"; break;
    case KIND_ABOUT:   w.w=300; w.h=178; w.body=0xF4F0E8; w.title="About";   break;
    case KIND_SYSMON:  w.w=320; w.h=272; w.body=0xF0F4F8; w.title="Monitor"; break;
    default:           w.w=240; w.h=150; w.body=0xF4F0E8; w.title="Window";  break;
    }
    windows[win_count++] = w;
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void desktop_run(void) {
    screen_w = fb_width();
    screen_h = fb_height();
    backbuffer = kmalloc((size_t)screen_w * screen_h * 4);
    scenebuf   = kmalloc((size_t)screen_w * screen_h * 4);
    fb_set_target(backbuffer);
    load_wallpaper();                    /* WALL.PNG from disk, else the gradient */
    start_y = screen_h - TASKBAR_H + 5;

    windows[win_count++] = (window_t){ 60, 70, 360, 290, 0xF0F0F0, "Welcome", KIND_WELCOME, 0, 0,0,0,0,0,0,0, 0,{0},0, 0 };
    windows[win_count++] = (window_t){ 60, 300, 500, 200, 0xE8ECF4, "Files", KIND_FILES, 0, 0,0,0,0,0,0,0, 0,{0},0, 0 };
    app_spawn_named("shell");           /* a real ring-3 shell (WM gives it a
                                         * window below; spawn more via Apps) */

    int dragging = -1, resizing = -1, gdx = 0, gdy = 0;
    int selecting = -1;                  /* window index whose text we're drag-selecting (terminal) */
    int bselecting = -1;                 /* window index whose text we're drag-selecting (browser) */
    int sbdrag = -1;                     /* window index whose terminal scrollbar we're dragging */
    int bsbdrag = -1;                    /* window index whose browser scrollbar we're dragging */
    int prev_btn = 0, prev_x = -1, prev_y = -1;
    uint64_t last_sec = (uint64_t)-1;
    uint64_t last_tb_click = 0; int last_tb_x = -100, last_tb_y = -100;  /* double-click-to-maximize */
    uint64_t last_body_click = 0; int last_body_x = -100, last_body_y = -100;  /* double-click-to-word-select */
    int drag_ox = 0, drag_oy = 0;        /* where a title-bar drag began (move-threshold + restore) */

    render_scene();
    present_frame();
    for (;;) {
        int dirty = 0;
        usb_tablet_poll();
        usb_kbd_poll();          /* feed any USB-keyboard keystrokes into the input queue */

        /* give any newly-spawned program a window — but only while there's a free
         * window slot. At the cap, leave the app PENDING (don't drain it) so it isn't
         * dropped-and-leaked; it gets a window once one closes (make_app_window's own
         * cap check stays as a belt-and-braces guard). */
        app_t *na;
        while (win_count < MAX_WINDOWS && (na = app_take_pending())) { make_app_window(na); dirty = 1; }

        /* open browser windows requested by the shell (`browse <url>`) */
        char burl[160];
        while (app_take_browse(burl, sizeof(burl))) { spawn_browser(burl); dirty = 1; }

        /* let any browser finish an async page load (parsed on this thread) */
        for (int i = 0; i < win_count; i++)
            if (windows[i].kind == KIND_BROWSER && windows[i].app) {
                browser_t *b = (browser_t *)windows[i].app;
                if (browser_poll(b)) dirty = 1;
                windows[i].title = browser_title(b);   /* keep title in sync */
            }

        /* repaint promptly when an app produced output (typing, clock, games) */
        for (int i = 0; i < win_count; i++)
            if (windows[i].kind == KIND_APP && windows[i].app) {
                if (app_dirty_clear((app_t *)windows[i].app)) dirty = 1;
                /* a graphics-mode app sizes its window to its canvas (+ borders) */
                uint32_t *cb; int gw, gh;
                if (app_gfx_get((app_t *)windows[i].app, &cb, &gw, &gh)) {
                    int s = gfx_scale(gw, gh), dw = s * gw + 12, dh = s * gh + TITLEBAR_H + 12;
                    if (windows[i].w != dw || windows[i].h != dh) {
                        windows[i].w = dw; windows[i].h = dh;
                        if (windows[i].x + dw > screen_w) windows[i].x = screen_w - dw;
                        if (windows[i].y + dh > screen_h - TASKBAR_H) windows[i].y = screen_h - TASKBAR_H - dh;
                        if (windows[i].x < 0) windows[i].x = 0;
                        if (windows[i].y < 0) windows[i].y = 0;
                        dirty = 1;
                    }
                }
            }

        int k;
        while ((k = input_trygetchar()) >= 0) {
            if (ctx_open) {                     /* context menu (either kind) is modal: Esc closes it, swallow */
                if (k == 27) { ctx_open = 0; dirty = 1; }   /* the rest so no F-key reorders under it */
                continue;
            }
            if (k == 0x1D) {                    /* F1: toggle the keyboard-shortcut help overlay */
                int o = help_open; close_overlays(); help_open = !o; dirty = 1;   /* one overlay at a time */
                continue;
            }
            if (help_open) {                    /* help overlay has focus: Esc/F1 close, swallow the rest */
                if (k == 27) { help_open = 0; dirty = 1; }
                continue;
            }
            if (k == 0x1E) {                    /* F7: toggle the Alt-Tab window switcher overlay */
                int o = sw_open; close_overlays(); sw_open = !o;          /* one overlay at a time */
                if (sw_open) sw_sel = win_count - 1;                      /* start on the focused (topmost) window */
                dirty = 1;
                continue;
            }
            if (sw_open) {                      /* switcher has keyboard focus while open */
                if (win_count > 0) {
                    if (k == 0x11 || k == 0x13)      sw_sel = (sw_sel + win_count - 1) % win_count;   /* up/left -> prev */
                    else if (k == 0x12 || k == 0x14) sw_sel = (sw_sel + 1) % win_count;               /* down/right -> next */
                    else if (k == '\n' || k == '\r') {                    /* Enter: raise + restore it */
                        raise_window(sw_sel);
                        dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1;   /* the array reordered: drop any active gesture */
                        sw_open = 0;
                    }
                    else if (k == 27) sw_open = 0;                        /* Esc: cancel */
                } else sw_open = 0;             /* nothing to switch to */
                dirty = 1;
                continue;                       /* swallow all keys while the switcher is up */
            }
            if (k == 0x19) {                    /* F9: toggle the Apps menu (keyboard) */
                int o = menu_open; close_overlays(); menu_open = !o; menu_sel = 0; dirty = 1;   /* one overlay at a time */
                continue;
            }
            if (menu_open) {                    /* menu has keyboard focus while open */
                if (k == 0x11) { menu_sel = (menu_sel + MENU_N - 1) % MENU_N; dirty = 1; }      /* up */
                else if (k == 0x12) { menu_sel = (menu_sel + 1) % MENU_N; dirty = 1; }          /* down */
                else if (k == 0x13) { menu_sel = (menu_sel + MENU_N - MENU_PERCOL) % MENU_N; dirty = 1; } /* left col */
                else if (k == 0x14) { menu_sel = (menu_sel + MENU_PERCOL) % MENU_N; dirty = 1; }          /* right col */
                else if (k == '\n' || k == '\r') { int s = menu_sel; menu_open = 0; dirty = 1;
                                                   spawn_app(menu[s].kind, menu[s].prog); }
                else if (k == 27) { menu_open = 0; dirty = 1; }                                 /* Esc */
                else if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) {
                    /* type-to-jump: hop to the next menu item whose label starts with this
                     * letter (wrapping), so a 34-entry menu is fast from the keyboard. */
                    int c = lc_ascii(k);
                    for (int n = 1; n <= MENU_N; n++) {
                        int j = (menu_sel + n) % MENU_N;
                        if (lc_ascii(menu[j].label[0]) == c) { menu_sel = j; dirty = 1; break; }
                    }
                }
                continue;                       /* swallow all keys while the menu is up */
            }
            if (k == 0x0E) {                    /* F2: cycle focus to the next window */
                if (win_count > 1) { raise_window(0); dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1; dirty = 1; }
                continue;
            }
            if (k == 0x0F) {                    /* F4: maximize/restore focused window */
                if (win_count > 0) { toggle_maximize(win_count - 1); dirty = 1; }
                continue;
            }
            if (k == 0x18) {                    /* F3: minimize the focused window to its chip */
                int vis = 0;                    /* never hide the LAST visible window */
                for (int i = 0; i < win_count; i++) if (!windows[i].minimized) vis++;
                if (vis > 1) {
                    windows[win_count - 1].minimized = 1;
                    sink_window(win_count - 1); /* focus falls to the window below; F2 or */
                    dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1;   /* the array reordered: drop any active gesture */
                    dirty = 1;                  /* its taskbar chip restores it (raise_window) */
                }
                continue;
            }
            if (k == 0x10 || k == 0x17) {       /* F5/F6: snap focused window left/right */
                if (win_count > 0) { snap_window(win_count - 1, k == 0x17); dirty = 1; }
                continue;
            }
            if (k == 0x1A) {                    /* F8: close the focused window */
                if (win_count > 0) {
                    int fi = win_count - 1;
                    if (windows[fi].kind == KIND_APP && windows[fi].app)
                        app_request_kill((app_t *)windows[fi].app);  /* app self-exits; reap loop drops the window */
                    else
                        remove_window(fi);       /* browser/files/etc: drop immediately */
                    dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1; dirty = 1;
                }
                continue;
            }
            if (k == 0x1C) {                    /* F12: screenshot to SHOT0.PNG, SHOT1.PNG, ... */
                static int shot_n;              /* auto-incrementing so a shot never overwrites the last */
                char name[16]; int q = 0;
                name[q++]='S'; name[q++]='H'; name[q++]='O'; name[q++]='T';
                int n = shot_n;
                if (n >= 100) name[q++] = (char)('0' + (n/100)%10);
                if (n >= 10)  name[q++] = (char)('0' + (n/10)%10);
                name[q++] = (char)('0' + n%10);
                name[q++]='.'; name[q++]='P'; name[q++]='N'; name[q++]='G'; name[q]=0;
                if (fb_save_png(name) == 0) { beep(1800, 30); if (shot_n < 999) shot_n++; }   /* PNG: ~9 KB vs ~576 KB BMP */
                continue;
            }
            if (win_count > 0) {                          /* with NO windows there's no focus target -> swallow the key; else windows[win_count-1] is windows[-1] = BSS garbage -> a wild app_key() call (mirrors the F-key guards above) */
                window_t *top = &windows[win_count - 1];
                if (top->minimized) { /* no visible focused window: swallow the key */ }
                else if (top->kind == KIND_APP && top->app) { app_sel_clear((app_t *)top->app); app_key((app_t *)top->app, (char)k); dirty = 1; }
                else if (top->kind == KIND_BROWSER && top->app) { browser_key((browser_t *)top->app, k); dirty = 1; }
                else if (top->kind == KIND_FILES) { files_key(top, k); dirty = 1; }
            }
        }

        /* raw make/break key events -> the focused app, if it opted into raw mode
         * (games like DOOM). Always drained so they never accumulate; delivered
         * only to a focused raw-mode app, else discarded. */
        {
            window_t *top = (win_count > 0) ? &windows[win_count - 1] : 0;
            int rawmode = top && !top->minimized && top->kind == KIND_APP &&
                          top->app && app_get_rawkb((app_t *)top->app);
            int ev;
            while ((ev = input_pop_raw()) >= 0)
                if (rawmode) app_key_raw((app_t *)top->app, (unsigned short)ev);
        }

        int mx = mouse_x(), my = mouse_y(), btn = mouse_buttons(), left = btn & 1;

        /* feed the focused graphics app its cursor position (canvas-relative,
         * undoing the integer upscale) + buttons, plus relative motion for
         * mouselook, so apps (and games like DOOM) can use the mouse */
        int rdx, rdy; mouse_read_rel(&rdx, &rdy);   /* always drain so it can't pile up */
        if (win_count > 0) {
            window_t *fw = &windows[win_count - 1];
            uint32_t *cb; int gw, gh;
            if (!fw->minimized && fw->kind == KIND_APP && fw->app &&
                app_gfx_get((app_t *)fw->app, &cb, &gw, &gh)) {
                int s = gfx_scale(gw, gh);
                int rx = (mx - (fw->x + 6)) / s, ry = (my - (fw->y + TITLEBAR_H + 6)) / s;
                if (rx < 0 || ry < 0 || rx >= gw || ry >= gh) { rx = -1; ry = -1; }
                app_set_mouse((app_t *)fw->app, rx, ry, btn);
                app_add_mouse_rel((app_t *)fw->app, rdx, rdy);
            }
        }

        /* Mouse wheel: scroll the topmost non-minimized window under the cursor.
         * wheel > 0 = rolled up (show content above); reused for the browser's
         * arrow-scroll and a text app's PgUp/PgDn scrollback. */
        int wheel = mouse_read_wheel();
        if (wheel && !menu_open && !help_open) {   /* drain the wheel always, but the Apps menu / help overlay is modal: don't scroll a window behind it */
            int up = wheel > 0, ticks = up ? wheel : -wheel;
            if (ticks > 8) ticks = 8;                    /* clamp a violent spin */
            for (int i = win_count - 1; i >= 0; i--) {
                window_t *w = &windows[i];
                if (w->minimized || !in_rect(mx, my, w->x, w->y, w->w, w->h)) continue;
                if (w->kind == KIND_BROWSER && w->app) {
                    for (int t = 0; t < ticks * 3; t++) browser_key((browser_t *)w->app, up ? 0x11 : 0x12);
                    dirty = 1;
                } else if (w->kind == KIND_APP && w->app) {
                    uint32_t *cb; int gw, gh;             /* text apps only (skip gfx canvases) */
                    if (!app_gfx_get((app_t *)w->app, &cb, &gw, &gh)) {
                        if (app_caret_hidden((app_t *)w->app))   /* editor etc.: scroll its own view a few lines */
                            for (int t = 0; t < ticks * 3; t++) app_key((app_t *)w->app, up ? 0x11 : 0x12);
                        else                                     /* terminal: PgUp/PgDn into the scrollback */
                            for (int t = 0; t < ticks; t++) app_key((app_t *)w->app, up ? 0x15 : 0x16);
                        dirty = 1;
                    }
                }
                break;                                   /* only the topmost window the cursor is over */
            }
        }

        /* Middle-click: paste the clipboard into the text app under the cursor
         * (X11 primary-selection style — no focus change needed). */
        if ((btn & 4) && !(prev_btn & 4)) {
            for (int i = win_count - 1; i >= 0; i--) {
                window_t *w = &windows[i];
                if (w->minimized || !in_rect(mx, my, w->x, w->y, w->w, w->h)) continue;
                if (w->kind == KIND_APP && w->app) {
                    uint32_t *cb; int gw, gh;
                    if (!app_gfx_get((app_t *)w->app, &cb, &gw, &gh)) { app_paste((app_t *)w->app); dirty = 1; }
                } else if (w->kind == KIND_BROWSER && w->app) {
                    char cbuf[512]; int n = clip_get(cbuf, sizeof cbuf);
                    browser_t *bp = (browser_t *)w->app;
                    raise_window(i);                     /* focus it so Enter submits after the paste */
                    if (n > 0) browser_paste(bp, cbuf, n);
                    dirty = 1;
                }
                break;
            }
        }

        /* Right-click: a window's TITLE BAR opens the window context menu (Maximize/
         * Restore, Minimize, Snap Left/Right, Close); browser CONTENT still copies a
         * link's URL; the EMPTY desktop (no window under the cursor) opens the
         * desktop-background menu (Show Desktop / Show All Windows / Change
         * Wallpaper). A right-click while any menu is open just dismisses it. */
        if ((btn & 2) && !(prev_btn & 2)) {
            if (ctx_open || menu_open || help_open || sw_open) {
                close_overlays(); dirty = 1;             /* a right-click dismisses any open overlay, no new menu */
            } else {
                int hit = 0;                             /* did the click land on a (visible) window? */
                for (int i = win_count - 1; i >= 0; i--) {
                    window_t *w = &windows[i];
                    if (w->minimized || !in_rect(mx, my, w->x, w->y, w->w, w->h)) continue;
                    hit = 1;
                    if (my < w->y + TITLEBAR_H) {        /* on the title bar -> open the window menu (ctx_kind 0) */
                        raise_window(i);                 /* normal title interaction: focus it first */
                        dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1;   /* array reordered */
                        ctx_kind = 0;
                        ctx_win = win_count - 1;          /* now topmost; the menu always targets the top window */
                        int ch = ctx_nrows() * CTX_ROW_H + 4, cw = ctx_w();
                        ctx_x = mx; if (ctx_x > screen_w - cw) ctx_x = screen_w - cw; if (ctx_x < 0) ctx_x = 0;
                        ctx_y = my; if (ctx_y > screen_h - TASKBAR_H - ch) ctx_y = screen_h - TASKBAR_H - ch; if (ctx_y < 0) ctx_y = 0;
                        ctx_open = 1; dirty = 1;
                    } else if (w->kind == KIND_BROWSER && w->app) {   /* browser content: copy a link's URL */
                        char lb[1024];
                        int n = browser_rclick((browser_t *)w->app, mx - w->x, my - (w->y + TITLEBAR_H), lb, sizeof lb);
                        if (n > 0) { clip_set(lb, n); dirty = 1; }
                    }
                    break;
                }
                /* fell through the loop with no window under the cursor -> desktop
                 * menu (only when over the desktop proper, not the taskbar) */
                if (!hit && my < screen_h - TASKBAR_H) {
                    ctx_kind = 1; ctx_win = -1;
                    int ch = ctx_nrows() * CTX_ROW_H + 4, cw = ctx_w();
                    ctx_x = mx; if (ctx_x > screen_w - cw) ctx_x = screen_w - cw; if (ctx_x < 0) ctx_x = 0;
                    ctx_y = my; if (ctx_y > screen_h - TASKBAR_H - ch) ctx_y = screen_h - TASKBAR_H - ch; if (ctx_y < 0) ctx_y = 0;
                    ctx_open = 1; dirty = 1;
                }
            }
        }

        if (left && !(prev_btn & 1) && help_open) {
            help_open = 0; dirty = 1;        /* the help overlay is modal: a click anywhere dismisses it */
        } else if (left && !(prev_btn & 1) && ctx_open) {
            /* the context menu is modal: a click on a row runs that action, a click
             * anywhere else just dismisses it. Either way the window beneath is NOT
             * actioned. Close the menu BEFORE acting (so Close/minimize can't act on
             * a stale popup). The window kind re-validates ctx_win (a window may have
             * been reaped since the menu opened) so a stale index is never used; the
             * desktop kind has no window dependency. */
            int row = ctx_row_at(mx, my), kind = ctx_kind, win = ctx_win;
            ctx_open = 0; dirty = 1;
            if (row >= 0) {
                int reordered = 0;
                if (kind == 1)                  /* desktop-background menu */
                    reordered = ctx_desktop_action(row);
                else if (win >= 0 && win < win_count)   /* window menu */
                    reordered = ctx_action(win, row);
                if (reordered)                  /* reordered/removed the array: drop any active gesture */
                    dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1;
            }
        } else if (left && !(prev_btn & 1)) {
            int ty = screen_h - TASKBAR_H, mh = MENU_PERCOL*MENU_ITEM_H + 4;
            int mw = MENU_COLS*MENU_W, my0 = ty - mh;
            if (menu_open) {
                if (in_rect(mx, my, start_x, my0, mw, mh)) {
                    int col = (mx - start_x) / MENU_W, row = (my - (my0 + 4)) / MENU_ITEM_H;
                    int idx = col * MENU_PERCOL + row;
                    if (col >= 0 && col < MENU_COLS && row >= 0 && idx >= 0 && idx < MENU_N)
                        spawn_app(menu[idx].kind, menu[idx].prog);
                }
                menu_open = 0; dirty = 1;
            } else if (in_rect(mx, my, start_x, start_y, start_w, start_h)) {
                menu_open = 1; menu_sel = 0; dirty = 1;  /* reset kbd highlight to top */
            } else if (my >= start_y) {                 /* the taskbar row */
                int clkx = screen_w - clk_pill_w() - 8;
                if (mx >= clkx) {                        /* clicking the clock opens the Calendar */
                    spawn_app(KIND_APP, "calendar"); dirty = 1;
                } else for (int i = 0; i < win_count; i++) {     /* else a window chip? */
                    int cx = TB_CHIPX0 + i * (TB_CHIPW + TB_CHIPGAP);
                    if (cx + TB_CHIPW > clkx - 8) break;
                    if (mx >= cx && mx < cx + TB_CHIPW) { raise_window(i); dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1; dirty = 1; break; }
                }
            } else {
                for (int i = win_count - 1; i >= 0; i--) {
                    window_t *w = &windows[i];
                    if (w->minimized) continue;                 /* hidden: not clickable */
                    if (!in_rect(mx, my, w->x, w->y, w->w, w->h)) continue;
                    if (in_rect(mx, my, w->x + w->w - 20, w->y + 7, 12, 12)) {
                        if (w->kind == KIND_APP && w->app)
                            app_request_kill((app_t *)w->app);  /* app self-exits; reap loop drops the window */
                        else
                            remove_window(i);                   /* browser/files/etc: drop immediately */
                    } else if (in_rect(mx, my, w->x + w->w - GRIP, w->y + w->h - GRIP, GRIP, GRIP)) {
                        raise_window(i); resizing = win_count - 1;
                    } else {
                        raise_window(i);
                        window_t *t = &windows[win_count - 1];
                        if (my < t->y + TITLEBAR_H) {
                            /* a second title-bar click within ~0.4s near the first =
                             * double-click -> maximize/restore (the F4 toggle). Otherwise
                             * begin a move drag and arm the double-click timer. */
                            uint64_t now = timer_ticks();
                            int near = (mx - last_tb_x < 8 && last_tb_x - mx < 8 &&
                                        my - last_tb_y < 8 && last_tb_y - my < 8);
                            if (now - last_tb_click < 40 && near) {
                                toggle_maximize(win_count - 1);
                                last_tb_click = 0;            /* consume: a 3rd click starts fresh */
                            } else {
                                dragging = win_count - 1; gdx = mx - t->x; gdy = my - t->y;
                                drag_ox = mx; drag_oy = my;
                                last_tb_click = now; last_tb_x = mx; last_tb_y = my;
                            }
                        }
                        else if (t->kind == KIND_BROWSER && t->app) {
                            int rbx = mx - t->x, rby = my - (t->y + TITLEBAR_H);
                            uint64_t now = timer_ticks();
                            int near = (mx - last_body_x < 8 && last_body_x - mx < 8 &&
                                        my - last_body_y < 8 && last_body_y - my < 8);
                            if (now - last_body_click < 40 && near) {     /* double-click: select the word */
                                char wb[256];
                                int n = browser_sel_word((browser_t *)t->app, rbx, rby, wb, sizeof wb);
                                if (n > 0) clip_set(wb, n);
                                last_body_click = 0;
                            } else if (browser_in_scrollbar((browser_t *)t->app, rbx, rby, t->w, t->h - TITLEBAR_H)) {
                                browser_scroll_track((browser_t *)t->app, rby, t->h - TITLEBAR_H);
                                bsbdrag = win_count - 1;
                            } else {
                                last_body_click = now; last_body_x = mx; last_body_y = my;
                                if (!browser_click((browser_t *)t->app, rbx, rby, t->w, t->h - TITLEBAR_H)) {
                                    browser_sel_begin((browser_t *)t->app, rbx, rby);   /* plain content: start text selection */
                                    bselecting = win_count - 1;
                                }
                            }
                        }
                        else if (t->kind == KIND_FILES)
                            files_click(t, my);          /* click a file row -> select + open it */
                        else if (t->kind == KIND_APP && t->app) {
                            uint32_t *cb; int gw, gh;    /* text app: scrollbar / word-select / drag-select */
                            if (!app_gfx_get((app_t *)t->app, &cb, &gw, &gh)) {
                                int px = t->x + 6, py = t->y + TITLEBAR_H + 6;
                                int sbx = px + app_cols() * font_width + 1, trackh = app_rows() * font_height;
                                if (mx >= sbx - 4 && mx <= sbx + 6 && my >= py && my < py + trackh) {
                                    app_scroll_frac((app_t *)t->app, my - py, trackh);   /* click the scrollbar */
                                    sbdrag = win_count - 1;
                                    dirty = 1; break;
                                }
                                int row = (my - py) / font_height, col = (mx - px) / font_width;
                                uint64_t now = timer_ticks();
                                int near = (mx - last_body_x < 8 && last_body_x - mx < 8 &&
                                            my - last_body_y < 8 && last_body_y - my < 8);
                                if (now - last_body_click < 40 && near) {     /* double-click: select the word */
                                    app_sel_word((app_t *)t->app, row, col);
                                    selecting = -1; last_body_click = 0;
                                } else {                                      /* single: begin a drag-selection */
                                    app_sel_begin((app_t *)t->app, row, col);
                                    selecting = win_count - 1;
                                    last_body_click = now; last_body_x = mx; last_body_y = my;
                                }
                            }
                        }
                    }
                    dirty = 1; break;
                }
            }
        } else if (!left) {
            /* aero-snap: release a dragged window against a screen edge to tile it
             * (top -> maximize, left/right -> that half). Only when it actually
             * moved (drag_ox/oy set on press) so a plain click never snaps. */
            if (dragging >= 0) {
                int moved = (mx - drag_ox > 3 || drag_ox - mx > 3 ||
                             my - drag_oy > 3 || drag_oy - my > 3);
                if (moved) {
                    if (my <= 2)                    { if (!windows[dragging].maximized) toggle_maximize(dragging); }
                    else if (mx <= 2)               snap_window(dragging, 0);
                    else if (mx >= screen_w - 2)    snap_window(dragging, 1);
                }
            }
            if (selecting >= 0) {                  /* finished a terminal text drag-selection: copy it */
                app_sel_commit((app_t *)windows[selecting].app); selecting = -1; dirty = 1;
            }
            if (bselecting >= 0) {                 /* finished a browser text drag-selection: copy it */
                char sbuf[2048];
                int n = browser_sel_commit((browser_t *)windows[bselecting].app, sbuf, sizeof sbuf);
                if (n > 0) clip_set(sbuf, n);
                bselecting = -1; dirty = 1;
            }
            dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1; sbdrag = -1; bsbdrag = -1; dirty = 1;
        }

        if (sbdrag >= 0 && left) {                 /* dragging the terminal scrollbar */
            window_t *w = &windows[sbdrag];
            int py = w->y + TITLEBAR_H + 6, trackh = app_rows() * font_height;
            app_scroll_frac((app_t *)w->app, my - py, trackh);
            dirty = 1;
        }
        if (bsbdrag >= 0 && left) {                 /* dragging the browser scrollbar */
            window_t *w = &windows[bsbdrag];
            browser_scroll_track((browser_t *)w->app, my - (w->y + TITLEBAR_H), w->h - TITLEBAR_H);
            dirty = 1;
        }
        if (selecting >= 0 && left) {              /* dragging a terminal text selection: extend it */
            window_t *w = &windows[selecting];
            int px = w->x + 6, py = w->y + TITLEBAR_H + 6;
            app_sel_extend((app_t *)w->app, (my - py) / font_height, (mx - px) / font_width);
            dirty = 1;
        }
        if (bselecting >= 0 && left) {             /* dragging a browser text selection: extend it */
            window_t *w = &windows[bselecting];
            browser_sel_extend((browser_t *)w->app, mx - w->x, my - (w->y + TITLEBAR_H));
            dirty = 1;
        }
        if (dragging >= 0 && left) {
            /* Only treat it as a move once the cursor actually leaves the press
             * point (a few px). A bare click on the title bar then does NOT move
             * or un-maximize the window -- which is what lets a double-click
             * maximize/restore land cleanly, and matches normal DE behavior. */
            int moved = (mx - drag_ox > 3 || drag_ox - mx > 3 ||
                         my - drag_oy > 3 || drag_oy - my > 3);
            if (moved) {
                window_t *w = &windows[dragging];
                if (w->maximized) {                   /* dragging a maximized window restores its */
                    toggle_maximize(dragging);        /* pre-max size, then re-grabs at the title  */
                    gdx = w->w / 2; gdy = TITLEBAR_H / 2; /* center so it follows the cursor */
                }
                w->x = mx - gdx; w->y = my - gdy;
                if (w->y < 0) w->y = 0;                                                       /* keep the title bar on-screen at the top */
                else if (w->y > screen_h - TASKBAR_H - TITLEBAR_H) w->y = screen_h - TASKBAR_H - TITLEBAR_H;   /* and above the taskbar (else it hides behind it -> mouse-unreachable) */
            }
        }
        if (resizing >= 0 && left) {
            window_t *w = &windows[resizing];
            int mw, mh; win_min(w, &mw, &mh);
            int nw = mx - w->x, nh = my - w->y;
            w->w = nw < mw ? mw : nw; w->h = nh < mh ? mh : nh;
            w->maximized = 0;                         /* a manual resize un-maximizes */
        }

        /* reap windows whose app has exited: free the app's task/stack/slot
         * (only once its task is fully off-CPU), then drop the window. If it
         * exited but isn't off-CPU yet, app_reap returns 0 — leave the window
         * for the next pass so we never drop it without reclaiming the slot. */
        for (int i = win_count - 1; i >= 0; i--)
            if (windows[i].kind == KIND_APP && !app_alive((app_t *)windows[i].app)) {
                if (!app_reap((app_t *)windows[i].app)) continue;
                remove_window(i); dirty = 1;
                dragging = resizing = selecting = bselecting = sbdrag = bsbdrag = -1;             /* the array shifted: drop any active gesture */
                if (ctx_open) ctx_open = 0;     /* a window vanished: close the menu so ctx_win can't go stale */
            }

        uint64_t sec = timer_ticks() / 100;
        if (sec != last_sec) { last_sec = sec; dirty = 1; }
        if (wallpaper_repaint) { wallpaper_repaint = 0; dirty = 1; }   /* `wallpaper` builtin swapped the bg */
        int moved = (mx != prev_x || my != prev_y || btn != prev_btn);
        if (moved && (dragging >= 0 || resizing >= 0)) dirty = 1;  /* drag moves the scene */

        if (dirty) { render_scene(); present_frame(); }  /* scene changed: full redraw + blit */
        else if (moved) present_cursor();                 /* cursor only: tiny rect blit */
        prev_x = mx; prev_y = my; prev_btn = btn;
        idle_hlt();    /* halt till the next IRQ; credits the wait to idle so /proc CPU% is real (M1361) */
    }
}
