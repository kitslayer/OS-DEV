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
#include "kheap.h"
#include "keyboard.h"
#include "timer.h"
#include "app.h"
#include "browser.h"
#include "net.h"
#include "vfs.h"
#include "rtc.h"
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
       KIND_BROWSER, KIND_SYSMON };

typedef struct {
    int x, y, w, h;
    uint32_t body;
    const char *title;
    int kind;
    void *app;            /* app_t* for KIND_APP windows */
    int maximized;        /* F4 toggle: filling the screen */
    int sx, sy, sw, sh;   /* saved geometry to restore from maximize */
    int fsel;             /* KIND_FILES: selected row (keyboard nav) */
    int minimized;        /* F3: hidden to its taskbar chip (must stay last:
                             existing positional struct literals zero-fill it) */
} window_t;

static window_t windows[MAX_WINDOWS];
static int win_count;
static uint32_t *backbuffer;
static uint32_t *scenebuf;          /* cached rendered scene (no cursor) */
static int screen_w, screen_h;
static int spawn_n, menu_open, menu_sel;   /* menu_sel: keyboard-highlighted item */
static int start_x = 8, start_y, start_w = 110, start_h = 24;

struct menu_item { const char *label; int kind; const char *prog; };
static const struct menu_item menu[] = {
    { "Browser", KIND_BROWSER, 0 }, { "Shell", KIND_APP, "shell" },
    { "Clock", KIND_APP, "clock" }, { "Calc", KIND_APP, "calc" },
    { "Snake", KIND_APP, "snake" }, { "Editor", KIND_APP, "editor" },
    { "2048", KIND_APP, "2048" }, { "Life", KIND_APP, "life" }, { "Mines", KIND_APP, "mines" },
    { "Tetris", KIND_APP, "tetris" }, { "Breakout", KIND_APP, "breakout" },
    { "Sudoku", KIND_APP, "sudoku" }, { "Maze", KIND_APP, "maze" }, { "Mandelbrot", KIND_APP, "mandel" },
    { "Hangman", KIND_APP, "hangman" }, { "Adventure", KIND_APP, "adv" },
    { "Tic-Tac-Toe", KIND_APP, "ttt" }, { "Blackjack", KIND_APP, "bj" }, { "Typing", KIND_APP, "typing" },
    { "Simon", KIND_APP, "simon" }, { "Connect 4", KIND_APP, "c4" },
    { "Wordle", KIND_APP, "wordle" },
    { "Graphics Demo", KIND_APP, "gfxdemo" },
    { "DOOM", KIND_APP, "doom" },
    { "Paint", KIND_APP, "paint" }, { "Piano", KIND_APP, "piano" }, { "Jukebox", KIND_APP, "jukebox" },
    { "Matrix", KIND_APP, "matrix" }, { "Calendar", KIND_APP, "calendar" },
    { "Monitor", KIND_SYSMON, 0 },
    { "Files", KIND_FILES, 0 }, { "Welcome", KIND_WELCOME, 0 },
    { "About", KIND_ABOUT, 0 },
};
/* CAPACITY: one column rendered upward from the taskbar fits ~30 entries at
 * 1024x768 (my0 = ty - (MENU_N*MENU_ITEM_H + 4) goes negative at N>=31, clipping
 * the top off-screen — graceful, no fault, but unreadable). Add a 2-column or
 * scrolling menu before exceeding that. Currently 29. */
#define MENU_N      (int)(sizeof(menu) / sizeof(menu[0]))
#define MENU_W      150
#define MENU_ITEM_H 24
#define TB_CHIPW    124                 /* taskbar window-chip width */
#define TB_CHIPGAP  6
#define TB_CHIPX0   (start_x + start_w + 10)

static void draw_text(int x, int y, const char *s, uint32_t fg) {
    for (int i = 0; s[i]; i++)
        fb_glyph_fg(x + i * font_width, y, s[i], fg);
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
/* Corner-pixel inset for a small rounded radius (index 0 = outermost row). */
static const int corner[] = { 4, 2, 1, 1 };
#define CORNER_R ((int)(sizeof(corner)/sizeof(corner[0])))

#define WP_TOP 0x183A5C
#define WP_BOT 0x081320
static int wp_h;
static uint32_t wallpaper_color(int y) { return lerp(WP_TOP, WP_BOT, y, wp_h - 1); }
static void u2(uint64_t v, char *o) { o[0]='0'+(v/10)%10; o[1]='0'+v%10; o[2]=0; }
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

static void draw_content(const window_t *w) {
    int bx = w->x + 8, by = w->y + TITLEBAR_H + 8;
    switch (w->kind) {
    case KIND_WELCOME: {
        const char *L[] = { "Welcome to OS-DEV!", "",
            "A from-scratch x86_64 OS", "with its own desktop.", "",
            "- drag the title bar to move", "- drag the corner to resize",
            "- Apps menu (click or F9) runs apps", "- F2 switch F4 max F5/6 tile  F12 shot" };
        for (unsigned i = 0; i < sizeof(L)/sizeof(L[0]); i++) draw_text(bx, by+i*18, L[i], 0x202028);
        break;
    }
    case KIND_FILES: {
        draw_text(bx, by, "FAT32 disk (/) - up/down, Enter opens:", 0x202028);
        static vfs_dirent e[256]; int n = vfs_list(e, 256);   /* static (BSS): ~18KB won't fit the 16KB guard-page-less stack; single-threaded render makes it safe. Browse ALL disk files, not just the first 32 (M421) */
        int rows = (w->h - TITLEBAR_H - 30) / 18;          /* rows that fit in the body */
        if (rows < 1) rows = 1;
        int top = 0;                                       /* scroll so the selection stays visible */
        if (w->fsel >= rows) top = w->fsel - rows + 1;
        for (int i = top; i < n && i < top + rows; i++) {
            int ry = by + 22 + (i - top)*18;
            if (i == w->fsel)                              /* highlight the selected row */
                fb_fill_rect(bx - 2, ry - 2, w->w - 14, 18, 0xCFE0F5);
            char line[48]; int p = 0; line[p++]=' '; line[p++]=' ';
            for (int j = 0; e[i].name[j] && p < 28; j++) line[p++] = e[i].name[j];
            while (p < 22) line[p++] = ' ';
            line[p++]='('; uint32_t s=e[i].size; char num[12]; int k=0;
            if (!s) num[k++]='0';
            while (s) { num[k++]='0'+s%10; s/=10; }
            while (k) line[p++]=num[--k];
            line[p++]='b'; line[p++]=')'; line[p]=0;
            draw_text(bx, ry, line, 0x303840);
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
        } else if (w->app) app_render((app_t *)w->app, bx - 2, by - 2);
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
        break;
    }
    case KIND_ABOUT: {
        const char *L[] = { "OS-DEV", "", "x86_64, built from scratch", "",
            "kernel . memory . tasks", "userspace . FAT32 . TCP",
            "TLS 1.3 . HTTPS web browser" };
        for (unsigned i = 0; i < sizeof(L)/sizeof(L[0]); i++) draw_text(bx, by + i*16, L[i], 0x202028);
        break;
    }
    default:
        draw_text(bx, by, w->title, 0x303840);
        break;
    }
}

static void draw_window(const window_t *w, int focused) {
    int x = w->x, y = w->y, ww = w->w, hh = w->h;

    /* soft drop shadow: two darkened, offset layers */
    darken(x + 8, y + 9, ww, hh, 62);
    darken(x + 4, y + 5, ww, hh, 78);

    /* body */
    fb_fill_rect(x, y, ww, hh, w->body);
    draw_content(w);

    /* gradient title bar (brighter when focused) with a top sheen line */
    uint32_t t0 = focused ? 0x5B9BF0 : 0x646B79;
    uint32_t t1 = focused ? 0x2C66D6 : 0x434A57;
    vgrad(x, y, ww, TITLEBAR_H, t0, t1);
    fb_fill_rect(x, y, ww, 1, lerp(t0, 0xFFFFFF, 1, 3));   /* highlight */
    draw_text(x + 10, y + (TITLEBAR_H - font_height) / 2 + 1, w->title, 0xFFFFFF);

    /* close button: a rounded-ish red chip */
    int cbx = x + ww - 21, cby = y + 6;
    vgrad(cbx, cby, 14, 14, 0xFF6B6B, 0xD63B3B);
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
            fb_pixel(x + i, y + j, wallpaper_color(y + j));
            fb_pixel(x + ww - 1 - i, y + j, wallpaper_color(y + j));
            fb_pixel(x + i, y + hh - 1 - j, wallpaper_color(y + hh - 1 - j));
            fb_pixel(x + ww - 1 - i, y + hh - 1 - j, wallpaper_color(y + hh - 1 - j));
        }
    }
}

/* Render the whole scene (wallpaper, windows, taskbar — but NOT the cursor)
 * into the cached scene buffer. This is the expensive part, so we only do it
 * when the scene actually changes; plain cursor moves reuse the cache. */
static void render_scene(void) {
    fb_set_target(scenebuf);
    wp_h = screen_h;
    vgrad(0, 0, screen_w, screen_h, WP_TOP, WP_BOT);        /* wallpaper */

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
    draw_text(start_x + 14, start_y + 4, "Apps", 0xFFFFFF);

    /* real-time clock (RTC) in a recessed pill on the right */
    struct rtc_time tm; rtc_now(&tm);
    char clk[9];
    u2(tm.hour, clk); clk[2]=':'; u2(tm.min, clk+3); clk[5]=':'; u2(tm.sec, clk+6); clk[8]=0;
    int clkw = 88, clkx = screen_w - clkw - 8;

    /* one chip per open window (the focused one — topmost — is highlighted) */
    for (int i = 0; i < win_count; i++) {
        int cx = TB_CHIPX0 + i * (TB_CHIPW + TB_CHIPGAP);
        if (cx + TB_CHIPW > clkx - 8) break;                  /* out of room */
        int foc = (i == win_count - 1);
        int mini = windows[i].minimized;                      /* hidden: dim its chip */
        vgrad(cx, start_y, TB_CHIPW, start_h, foc ? 0x3A78D8 : (mini ? 0x1B222E : 0x2A3344),
              foc ? 0x2C66D6 : 0x161D29);
        box(cx, start_y, TB_CHIPW, start_h, foc ? 0x4A90E2 : 0x33415A);
        char t[18]; int n = 0; const char *s = windows[i].title;
        while (s && s[n] && n < 15) { t[n] = s[n]; n++; } t[n] = 0;
        draw_text(cx + 8, start_y + 4, t, foc ? 0xFFFFFF : (mini ? 0x6B7689 : 0xAEB8C8));
    }
    fb_fill_rect(clkx, start_y, clkw, start_h, 0x10151E);
    box(clkx, start_y, clkw, start_h, 0x33415A);
    draw_text(clkx + 14, start_y + 4, clk, 0x9FC0F0);

    if (menu_open) {
        int mh = MENU_N * MENU_ITEM_H + 4, my0 = ty - mh;
        fb_fill_rect(start_x, my0, MENU_W, mh, 0x1E1E2A);
        box(start_x, my0, MENU_W, mh, 0x2D6CDF);
        for (int i = 0; i < MENU_N; i++) {
            int iy = my0 + 4 + i * MENU_ITEM_H;
            if (i == menu_sel)                                   /* keyboard highlight */
                fb_fill_rect(start_x + 2, iy, MENU_W - 4, MENU_ITEM_H, 0x2D4A8A);
            draw_text(start_x + 12, iy + 2, menu[i].label, i == menu_sel ? 0xFFFFFF : 0xD0D8F0);
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

/* Give a freshly-spawned app a window (called by the WM as it drains the
 * spawn queue, so apps launched from anywhere get a window here). */
static void make_app_window(app_t *a) {
    if (win_count >= MAX_WINDOWS) return;
    spawn_n++;
    int x = 150 + (spawn_n % 6) * 26, y = 60 + (spawn_n % 6) * 26;
    windows[win_count++] = (window_t){ x, y,
        app_cols()*font_width + 14, app_rows()*font_height + TITLEBAR_H + 14,
        0x0A0A0A, app_title(a), KIND_APP, a, 0,0,0,0,0,0,0 };  /* maximized,sx,sy,sw,sh,fsel,minimized */
}

/* Open a browser window at `url` (NULL -> its default). */
static void spawn_browser(const char *url) {
    if (win_count >= MAX_WINDOWS) return;
    spawn_n++;
    int x = 150 + (spawn_n % 6) * 26, y = 60 + (spawn_n % 6) * 26;
    windows[win_count++] = (window_t){ x, y, 620, 460, 0xFFFFFF, "Browser",
                                       KIND_BROWSER, browser_create(url), 0,0,0,0,0,0,0 };
}

/* Keyboard navigation for the read-only Files window: up/down move the
 * selection, Enter opens the highlighted file in a browser window (file:NAME). */
static void files_key(window_t *w, int k) {
    static vfs_dirent e[256]; int n = vfs_list(e, 256);   /* match the render cap; static (BSS), safe single-threaded (M421) */
    if (n <= 0) return;
    if (w->fsel >= n) w->fsel = n - 1;
    if (k == 0x11)       { if (w->fsel > 0)     w->fsel--; }   /* up   */
    else if (k == 0x12)  { if (w->fsel < n - 1) w->fsel++; }   /* down */
    else if (k == '\n' || k == '\r') {                         /* open in browser */
        const char *name = e[w->fsel].name;
        int len = 0; while (name[len]) len++;
        if (len > 0 && name[len-1] != '/') {                   /* skip directories */
            char url[32]; int p = 0; const char *pre = "file:";
            while (*pre) url[p++] = *pre++;
            for (int j = 0; name[j] && p < (int)sizeof(url) - 1; j++) url[p++] = name[j];
            url[p] = 0;
            spawn_browser(url);
        }
    }
}

static void spawn_app(int kind, const char *prog) {
    if (kind == KIND_APP)     { app_spawn_named(prog); return; }  /* WM drains it */
    if (kind == KIND_BROWSER) { spawn_browser(0); return; }
    if (win_count >= MAX_WINDOWS) return;
    spawn_n++;
    int x = 150 + (spawn_n % 6) * 26, y = 60 + (spawn_n % 6) * 26;
    window_t w = { 0 };
    w.x = x; w.y = y; w.kind = kind;
    switch (kind) {
    case KIND_FILES:   w.w=330; w.h=200; w.body=0xE8ECF4; w.title="Files";   break;
    case KIND_WELCOME: w.w=360; w.h=206; w.body=0xF0F0F0; w.title="Welcome"; break;
    case KIND_ABOUT:   w.w=300; w.h=160; w.body=0xF4F0E8; w.title="About";   break;
    case KIND_SYSMON:  w.w=320; w.h=240; w.body=0xF0F4F8; w.title="Monitor"; break;
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
    start_y = screen_h - TASKBAR_H + 5;

    windows[win_count++] = (window_t){ 60, 70, 360, 206, 0xF0F0F0, "Welcome", KIND_WELCOME, 0, 0,0,0,0,0,0,0 };
    windows[win_count++] = (window_t){ 60, 300, 330, 200, 0xE8ECF4, "Files", KIND_FILES, 0, 0,0,0,0,0,0,0 };
    app_spawn_named("shell");           /* a real ring-3 shell (WM gives it a
                                         * window below; spawn more via Apps) */

    int dragging = -1, resizing = -1, gdx = 0, gdy = 0;
    int prev_btn = 0, prev_x = -1, prev_y = -1;
    uint64_t last_sec = (uint64_t)-1;

    render_scene();
    present_frame();
    for (;;) {
        int dirty = 0;
        usb_tablet_poll();

        /* give any newly-spawned program a window */
        app_t *na;
        while ((na = app_take_pending())) { make_app_window(na); dirty = 1; }

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
            if (k == 0x19) {                    /* F9: toggle the Apps menu (keyboard) */
                menu_open = !menu_open; menu_sel = 0; dirty = 1;
                continue;
            }
            if (menu_open) {                    /* menu has keyboard focus while open */
                if (k == 0x11) { menu_sel = (menu_sel + MENU_N - 1) % MENU_N; dirty = 1; }      /* up */
                else if (k == 0x12) { menu_sel = (menu_sel + 1) % MENU_N; dirty = 1; }          /* down */
                else if (k == '\n' || k == '\r') { int s = menu_sel; menu_open = 0; dirty = 1;
                                                   spawn_app(menu[s].kind, menu[s].prog); }
                else if (k == 27) { menu_open = 0; dirty = 1; }                                 /* Esc */
                continue;                       /* swallow all keys while the menu is up */
            }
            if (k == 0x0E) {                    /* F2: cycle focus to the next window */
                if (win_count > 1) { raise_window(0); dragging = resizing = -1; dirty = 1; }
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
                    dragging = resizing = -1;   /* the array reordered: drop any active gesture */
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
                    dragging = resizing = -1; dirty = 1;
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
            window_t *top = &windows[win_count - 1];
            if (top->minimized) { /* no visible focused window: swallow the key */ }
            else if (top->kind == KIND_APP && top->app) { app_key((app_t *)top->app, (char)k); dirty = 1; }
            else if (top->kind == KIND_BROWSER && top->app) { browser_key((browser_t *)top->app, k); dirty = 1; }
            else if (top->kind == KIND_FILES) { files_key(top, k); dirty = 1; }
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

        if (left && !(prev_btn & 1)) {
            int ty = screen_h - TASKBAR_H, mh = MENU_N*MENU_ITEM_H + 4, my0 = ty - mh;
            if (menu_open) {
                if (in_rect(mx, my, start_x, my0, MENU_W, mh)) {
                    int idx = (my - (my0 + 4)) / MENU_ITEM_H;
                    if (idx >= 0 && idx < MENU_N) spawn_app(menu[idx].kind, menu[idx].prog);
                }
                menu_open = 0; dirty = 1;
            } else if (in_rect(mx, my, start_x, start_y, start_w, start_h)) {
                menu_open = 1; menu_sel = 0; dirty = 1;  /* reset kbd highlight to top */
            } else if (my >= start_y) {                 /* a taskbar window chip? */
                int clkx = screen_w - 88 - 8;
                for (int i = 0; i < win_count; i++) {
                    int cx = TB_CHIPX0 + i * (TB_CHIPW + TB_CHIPGAP);
                    if (cx + TB_CHIPW > clkx - 8) break;
                    if (mx >= cx && mx < cx + TB_CHIPW) { raise_window(i); dragging = resizing = -1; dirty = 1; break; }
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
                        if (my < t->y + TITLEBAR_H) { dragging = win_count - 1; gdx = mx - t->x; gdy = my - t->y; }
                        else if (t->kind == KIND_BROWSER && t->app)
                            browser_click((browser_t *)t->app, mx - t->x,
                                          my - (t->y + TITLEBAR_H), t->w, t->h - TITLEBAR_H);
                    }
                    dirty = 1; break;
                }
            }
        } else if (!left) {
            dragging = resizing = -1;
        }

        if (dragging >= 0 && left) {
            windows[dragging].x = mx - gdx; windows[dragging].y = my - gdy;
            windows[dragging].maximized = 0;          /* a manual move un-maximizes */
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
                dragging = resizing = -1;             /* the array shifted: drop any active gesture */
            }

        uint64_t sec = timer_ticks() / 100;
        if (sec != last_sec) { last_sec = sec; dirty = 1; }
        int moved = (mx != prev_x || my != prev_y || btn != prev_btn);
        if (moved && (dragging >= 0 || resizing >= 0)) dirty = 1;  /* drag moves the scene */

        if (dirty) { render_scene(); present_frame(); }  /* scene changed: full redraw + blit */
        else if (moved) present_cursor();                 /* cursor only: tiny rect blit */
        prev_x = mx; prev_y = my; prev_btn = btn;
        __asm__ volatile("hlt");
    }
}
