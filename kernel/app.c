/*
 * app.c — userspace apps as windowed, isolated, preemptive processes.
 *
 * Each app: a fresh address space (M21), the shell ELF loaded into it, a user
 * stack, and a kernel task whose trampoline drops to ring 3. The app's text
 * output lands in a character grid (app->grid); its keyboard input waits in a
 * small queue the window manager fills when the app's window is focused.
 *
 * The syscall dispatcher routes write/read/getpid/exit here, acting on
 * whichever app owns the task that trapped (task_self()->proc).
 */
#include "app.h"
#include "task.h"
#include "interrupts.h"   /* struct registers, for ring-3 signal delivery */
#include "vmm.h"
#include "pmm.h"
#include "elf.h"
#include "measure.h"
#include "fb.h"
#include "font.h"
#include "string.h"
#include "vfs.h"
#include "kheap.h"
#include "complete.h"
#include "console.h"   /* kprintf — log app-launch failures (don't fail silently) */
#include <stdint.h>

#define APP_COLS 44
#define APP_ROWS 17
#define SB_ROWS  48          /* scrollback: ~3 screens of history */
#define IQ_SIZE  128
#define MAX_APPS 8
#define HIST_N   32          /* command-history depth (up/down recall) */
#define CLIP_MAX 2048        /* system clipboard + per-app paste buffer size */

#define USTACK_BASE  0x50000000ull
#define USTACK_PAGES 128             /* 512 KiB user stack — DOOM's BSP renderer recurses deeply */

/* Userspace heap: grows up from 1 GiB + 64 MiB (clear of any app image, which
 * loads at 1 GiB and is at most a couple of MiB) toward the stack at 0x50000000.
 * That leaves ~192 MiB of per-process heap virtual space for malloc (sbrk). */
#define UHEAP_BASE   0x44000000ull
#define UHEAP_LIMIT  USTACK_BASE

struct app {
    int         used;
    int         pid;
    task_t     *task;
    const char *title;
    char        titlebuf[24];            /* persistent copy of the title */
    uint64_t cr3, entry, ustack;
    uint64_t heap_end;                   /* current program break (0 = not yet started) */
#define APP_MAXVMA 16
    struct { uint64_t start, len; } vma[APP_MAXVMA];  /* mmap'd demand-paged regions */
    int      nvma;
    uint64_t mmap_next;                  /* bump allocator for mmap addresses */
#define APP_NSIG 32
    uint64_t sig_handler[APP_NSIG];      /* ring-3 signal handlers (0 = none) */
    uint64_t sig_restorer;               /* ulib trampoline that calls sigreturn */
    struct registers sig_saved;          /* pre-signal context, restored by sigreturn */
    int      sig_in;                     /* 1 while a handler runs (no nesting) */
    volatile int pending_sig;            /* a signal raised asynchronously (e.g. Ctrl-C->SIGINT), delivered on the next return to ring 3 */
    int      traced;                     /* 1 = log each syscall to dmesg (strace), toggled via /proc/<pid>/ctl */
    uint32_t *gfx;                       /* graphics-mode pixel canvas (kernel heap), or NULL */
    int       gfx_w, gfx_h;              /* canvas dimensions (valid when gfx != NULL) */
    int       rawkb;                     /* raw keyboard mode (games get make/break events) */
    volatile unsigned short rawiq[64];   /* raw key-event queue (WM fills, app drains) */
    volatile int rqh, rqt;
    volatile int ms_x, ms_y, ms_btn;     /* cursor relative to the gfx canvas (-1 outside) + buttons */
    volatile int ms_dx, ms_dy;           /* accumulated relative motion (mouselook) */
    char     grid[APP_ROWS][APP_COLS];
    uint8_t  gcol[APP_ROWS][APP_COLS];   /* per-cell colour (palette index, 0 = default) for the live grid */
    uint8_t  curcol;                     /* colour applied to chars printed now (set via SYS_setcolor) */
    uint8_t  esc;                        /* ANSI escape state: 0 normal, 1 saw ESC, 2 in CSI */
    uint8_t  csilen;                     /* bytes buffered in csi[] */
    char     csi[24];                    /* CSI parameter bytes (between '[' and the final letter) */
    int      cx, cy;
    char     sb[SB_ROWS][APP_COLS];      /* scrollback: lines that scrolled off */
    int      sb_count;                   /* how many scrollback lines are stored */
    int      view;                       /* rows scrolled up from the live bottom */
    char     iq[IQ_SIZE];
    volatile int ih, it;
    volatile int exited;
    volatile int kill;                   /* WM asked this app to close: it self-exits at its input wait */
    char     hist[HIST_N][96];           /* recent input lines (for up/down) */
    int      hist_n, hist_pos;
    volatile int gdirty;                 /* grid changed -> WM should repaint */
    int      caret_off;                  /* 1 = suppress the system caret (app draws its own) */
    int      sel_on;                     /* a text selection is shown/in progress */
    int      sel_r0, sel_c0;             /* selection anchor (visible-grid cell) */
    int      sel_r1, sel_c1;             /* selection end (visible-grid cell) */
    char     pastebuf[CLIP_MAX];         /* middle-click paste text, drained before the key queue */
    volatile int paste_len, paste_pos;   /* so a long paste isn't capped by the small key queue */
    char     launch_arg[128];            /* optional launch argument (e.g. a filename for the editor) */
    uint32_t promises;                   /* pledge() promise bitmask (valid once pledged) */
    int      pledged;                    /* 1 once pledge() has been called (then promises are enforced) */
#define APP_NUNVEIL 8
    struct { char path[48]; uint8_t perms; } uv[APP_NUNVEIL];  /* unveil() allowed path prefixes */
    int      nuv;                        /* number of unveil entries */
    int      uv_active;                  /* 1 once unveil() has been called (then file paths are checked) */
    int      uv_locked;                  /* 1 after unveil(NULL): no more unveils accepted */
};

static struct app apps[MAX_APPS];
static int next_pid = 100;
static char g_pend_arg[128];             /* arg for the next app_spawn, copied into its launch_arg */
static int  g_have_pend;
/* A pending "jail" for the next app_spawn (M1088): pledge promises + an optional
 * unveil prefix applied to the child BEFORE it runs (a parent-enforced sandbox). */
static int      g_pend_jail;
static uint32_t g_jail_promises;
static char     g_jail_path[64];

/* text-colour palette for apps (index 0 = the default green, so an app that never
 * calls SYS_setcolor renders byte-identically). Vivid hues on the dark app background. */
static const uint32_t app_palette[16] = {
    0x33FF66, 0xEAEAEA, 0xFF5555, 0xFFE048, 0x44E0FF, 0xFF6CE0, 0x6E9CFF, 0xFF9A3C,
    0x9098A0, 0xB6FF4A, 0x2FE0C0, 0xB98CFF, 0xE8C040, 0xFF7A5C, 0x40C0FF, 0x6CFFB0,
};

/* apps awaiting a window from the window manager */
static struct app *pending[MAX_APPS];
static int pend_h, pend_t;

/* the embedded programs (see kernel/asm/user_blob.asm) */
extern char shell_elf_start[], clock_elf_start[], calc_elf_start[], snake_elf_start[],
            editor_elf_start[], g2048_elf_start[], life_elf_start[], tetris_elf_start[],
            breakout_elf_start[], mines_elf_start[], sudoku_elf_start[], calendar_elf_start[],
            timer_elf_start[],
            mandel_elf_start[], piano_elf_start[], maze_elf_start[], adv_elf_start[],
            matrix_elf_start[], paint_elf_start[], hangman_elf_start[], jukebox_elf_start[],
            ttt_elf_start[], bj_elf_start[], typing_elf_start[], simon_elf_start[],
            c4_elf_start[], wordle_elf_start[], gfxdemo_elf_start[],
            scene3d_elf_start[], terrain_elf_start[], demoscene_elf_start[], doom_elf_start[],
            quake_elf_start[], nes_elf_start[], reversi_elf_start[], lights_elf_start[],
            fifteen_elf_start[], mastermind_elf_start[], pong_elf_start[], halflife_elf_start[],
            memory_elf_start[], sokoban_elf_start[], battleship_elf_start[], pig_elf_start[],
            raycast_elf_start[], tron_elf_start[], spaceinv_elf_start[], asteroids_elf_start[],
            flappy_elf_start[], gb_elf_start[], lander_elf_start[], yahtzee_elf_start[],
            checkers_elf_start[], gomoku_elf_start[], frogger_elf_start[],
            chess_elf_start[], vpoker_elf_start[], mancala_elf_start[],
            dotsbox_elf_start[], missile_elf_start[], pacman_elf_start[],
            solitaire_elf_start[], gems_elf_start[], columns_elf_start[], freecell_elf_start[],
            spider_elf_start[], sandbox_elf_start[], forth_elf_start[], cc_elf_start[];
static const struct { const char *name; char *elf; const char *title; } progs[] = {
    { "shell",  shell_elf_start,  "Shell"  },
    { "clock",  clock_elf_start,  "Clock"  },
    { "calc",   calc_elf_start,   "Calc"   },
    { "snake",  snake_elf_start,  "Snake"  },
    { "editor", editor_elf_start, "Editor" },
    { "2048",   g2048_elf_start,  "2048"   },
    { "life",   life_elf_start,   "Life"   },
    { "tetris", tetris_elf_start, "Tetris" },
    { "breakout", breakout_elf_start, "Breakout" },
    { "mines",  mines_elf_start,  "Mines"  },
    { "sudoku", sudoku_elf_start, "Sudoku" },
    { "calendar", calendar_elf_start, "Calendar" },
    { "timer",  timer_elf_start,  "Timer" },
    { "mandel", mandel_elf_start, "Mandelbrot" },
    { "piano",  piano_elf_start,  "Piano" },
    { "maze",   maze_elf_start,   "Maze" },
    { "adv",    adv_elf_start,    "Adventure" },
    { "matrix", matrix_elf_start, "Matrix" },
    { "paint",  paint_elf_start,  "Paint" },
    { "hangman", hangman_elf_start, "Hangman" },
    { "jukebox", jukebox_elf_start, "Jukebox" },
    { "ttt",    ttt_elf_start,    "Tic-Tac-Toe" },
    { "bj",     bj_elf_start,     "Blackjack" },
    { "typing", typing_elf_start, "Typing Test" },
    { "simon",  simon_elf_start,  "Simon" },
    { "c4",     c4_elf_start,     "Connect Four" },
    { "wordle", wordle_elf_start, "Wordle" },
    { "gfxdemo", gfxdemo_elf_start, "Graphics Demo" },
    { "scene3d", scene3d_elf_start, "3D Engine" },
    { "terrain", terrain_elf_start, "Terrain" },
    { "demoscene", demoscene_elf_start, "Demoscene" },
    { "doom",   doom_elf_start,   "DOOM" },
    { "quake",  quake_elf_start,  "Quake" },
    { "nes",    nes_elf_start,    "NES" },
    { "reversi", reversi_elf_start, "Reversi" },
    { "lights", lights_elf_start, "Lights Out" },
    { "fifteen", fifteen_elf_start, "15 Puzzle" },
    { "mastermind", mastermind_elf_start, "Mastermind" },
    { "pong",   pong_elf_start,   "Pong" },
    { "halflife", halflife_elf_start, "Half-Life" },
    { "memory", memory_elf_start, "Memory" },
    { "sokoban", sokoban_elf_start, "Sokoban" },
    { "battleship", battleship_elf_start, "Battleship" },
    { "pig",    pig_elf_start,    "Pig" },
    { "raycast", raycast_elf_start, "Raycaster" },
    { "tron",   tron_elf_start,   "Tron" },
    { "spaceinv", spaceinv_elf_start, "Space Invaders" },
    { "asteroids", asteroids_elf_start, "Asteroids" },
    { "flappy", flappy_elf_start, "Flappy" },
    { "gb",     gb_elf_start,     "Game Boy" },
    { "lander", lander_elf_start, "Lunar Lander" },
    { "yahtzee", yahtzee_elf_start, "Yahtzee" },
    { "checkers", checkers_elf_start, "Checkers" },
    { "gomoku", gomoku_elf_start, "Gomoku" },
    { "frogger", frogger_elf_start, "Frogger" },
    { "chess", chess_elf_start, "Chess" },
    { "vpoker", vpoker_elf_start, "Video Poker" },
    { "mancala", mancala_elf_start, "Mancala" },
    { "dotsbox", dotsbox_elf_start, "Dots and Boxes" },
    { "missile", missile_elf_start, "Missile Command" },
    { "pacman", pacman_elf_start, "Pac-Man" },
    { "solitaire", solitaire_elf_start, "Solitaire" },
    { "gems",   gems_elf_start,   "Gems" },
    { "columns", columns_elf_start, "Columns" },
    { "freecell", freecell_elf_start, "FreeCell" },
    { "spider", spider_elf_start, "Spider" },
    { "sandbox", sandbox_elf_start, "Sandbox (pledge demo)" },
    { "forth", forth_elf_start, "Forth" },
    { "cc", cc_elf_start, "C Compiler" },
};
#define NPROGS (int)(sizeof(progs)/sizeof(progs[0]))

extern void enter_user(uint64_t entry, uint64_t ustack);

int app_cols(void) { return APP_COLS; }
int app_rows(void) { return APP_ROWS; }
const char *app_title(app_t *a) { return a->title; }
const char *app_arg(app_t *a) { return a ? a->launch_arg : ""; }          /* /proc/<pid>/cmdline */
void       *app_task(app_t *a) { return a ? (void *)a->task : 0; }        /* the task_t*, for /proc/<pid>/ctl stop/cont */
uint64_t    app_cr3(app_t *a) { return a ? a->cr3 : 0; }                  /* the app's address space, for /proc/<pid>/wss */
uint64_t    app_heap_bytes(app_t *a) { return (a && a->heap_end) ? a->heap_end - UHEAP_BASE : 0; }
int         app_vma_count(app_t *a) { return a ? a->nvma : 0; }

/* Format the app's user-space memory map (/proc/<pid>/maps): each region as a
 * "0xSTART-0xEND perm [label]" line, like Linux. Bounded by `max`. */
static int maps_hex(char *b, int p, int max, uint64_t v) {
    char t[16]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
    if (p < max - 1) b[p++] = '0';
    if (p < max - 1) b[p++] = 'x';
    while (n > 0 && p < max - 1) b[p++] = t[--n];
    return p;
}
static int maps_str(char *b, int p, int max, const char *s) {
    while (*s && p < max - 1) b[p++] = *s++;
    return p;
}
int app_format_maps(app_t *a, char *b, int max) {
    if (!a || max <= 0) return 0;
    int p = 0;
    if (a->heap_end > UHEAP_BASE) {     /* the program break heap */
        p = maps_hex(b, p, max, UHEAP_BASE); p = maps_str(b, p, max, "-");
        p = maps_hex(b, p, max, a->heap_end); p = maps_str(b, p, max, " rw-  [heap]\n");
    }
    for (int i = 0; i < a->nvma; i++) {  /* demand-paged mmap regions */
        p = maps_hex(b, p, max, a->vma[i].start); p = maps_str(b, p, max, "-");
        p = maps_hex(b, p, max, a->vma[i].start + a->vma[i].len); p = maps_str(b, p, max, " rw-  [mmap]\n");
    }
    p = maps_hex(b, p, max, USTACK_BASE);   /* the user stack region */
    p = maps_str(b, p, max, "  rw-  [stack]\n");
    if (p < max) b[p] = 0;
    return p;
}

static struct app *cur(void) { return (struct app *)task_self()->proc; }

/* --- pledge() sandbox (M1074) --------------------------------------------- */
app_t *app_current(void) { return cur(); }

/* Restrict the calling app's promises. Monotonic, like OpenBSD's pledge: the
 * first call sets the set; later calls may only DROP promises (the new mask
 * must be a subset of the current), never regain them. Returns 0 / -1. */
int app_pledge(app_t *a, uint32_t mask) {
    if (!a) return -1;
    if (a->pledged && (mask & ~a->promises)) return -1;   /* tried to add a promise back */
    a->promises = mask;
    a->pledged  = 1;
    return 0;
}
int      app_is_pledged(app_t *a) { return a && a->pledged; }
uint32_t app_promises(app_t *a)   { return a ? a->promises : 0; }

/* The promise name <-> bit table — the user ABI shared by SYS_pledge parsing
 * and /proc/<pid>/status formatting. */
static const struct { const char *name; uint32_t bit; } pledge_tab[] = {
    {"stdio",PL_STDIO},{"rpath",PL_RPATH},{"wpath",PL_WPATH},{"inet",PL_INET},
    {"gfx",PL_GFX},{"proc",PL_PROC},{"vm",PL_VM},{"power",PL_POWER},
};
#define PLEDGE_NTAB (int)(sizeof(pledge_tab)/sizeof(pledge_tab[0]))

int app_pledge_parse(const char *s, uint32_t *out) {
    uint32_t mask = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        const char *w = s; int len = 0;
        while (s[len] && s[len] != ' ' && s[len] != '\t') len++;
        s += len;
        int matched = 0;
        for (int i = 0; i < PLEDGE_NTAB; i++) {
            const char *n = pledge_tab[i].name; int j = 0;
            while (j < len && n[j] && n[j] == w[j]) j++;
            if (j == len && n[j] == 0) { mask |= pledge_tab[i].bit; matched = 1; break; }
        }
        if (!matched) return -1;                     /* unknown promise name */
    }
    *out = mask;
    return 0;
}

int app_pledge_format(uint32_t mask, char *buf, int max) {
    int p = 0;
    for (int i = 0; i < PLEDGE_NTAB; i++)
        if (mask & pledge_tab[i].bit) {
            if (p && p < max - 1) buf[p++] = ' ';
            const char *n = pledge_tab[i].name;
            while (*n && p < max - 1) buf[p++] = *n++;
        }
    if (p < max) buf[p] = 0;
    return p;
}

/* --- unveil(): restrict which filesystem paths a process can touch -----------
 * Like OpenBSD's unveil: before the first call every path is visible; the first
 * unveil() flips the process to "only the unveiled prefixes are reachable", with
 * per-prefix r/w permission. A denied access fails (-1, as if absent) — it does
 * NOT kill (that's pledge's job). unveil(NULL) locks the set. */
uint32_t app_unveil_parse(const char *s) {
    uint32_t b = 0;
    for (; s && *s; s++) {
        if (*s == 'r' || *s == 'R') b |= UV_R;
        else if (*s == 'w' || *s == 'W' || *s == 'c' || *s == 'C') b |= UV_W;  /* c(reate) implies write */
    }
    return b;
}

int app_unveil(app_t *a, const char *path, uint32_t perms) {
    if (!a) return -1;
    if (!path || !path[0]) { a->uv_locked = 1; a->uv_active = 1; return 0; }  /* unveil(NULL): lock */
    if (a->uv_locked || a->nuv >= APP_NUNVEIL) return -1;
    int i = 0; while (path[i] && i < (int)sizeof a->uv[0].path - 1) { a->uv[a->nuv].path[i] = path[i]; i++; }
    a->uv[a->nuv].path[i] = 0;
    a->uv[a->nuv].perms = (uint8_t)perms;
    a->nuv++;
    a->uv_active = 1;
    return 0;
}

static int uv_ci(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
/* `prefix` matches `path` if it is path itself or a parent directory of it. */
static int uv_match(const char *prefix, const char *path) {
    int i = 0;
    while (prefix[i] && uv_ci(prefix[i]) == uv_ci(path[i])) i++;
    if (prefix[i] != 0) return 0;                       /* prefix not fully consumed */
    return path[i] == 0 || path[i] == '/';              /* exact, or a sub-path boundary */
}

int app_unveil_ok(app_t *a, const char *path, int need_write) {
    if (!a || !a->uv_active) return 1;                  /* unveil never called -> all allowed */
    for (int i = 0; i < a->nuv; i++)
        if (uv_match(a->uv[i].path, path) &&
            (need_write ? (a->uv[i].perms & UV_W) : (a->uv[i].perms & UV_R)))
            return 1;
    return 0;
}

/* ---- text grid ---- */
static void grid_clear(struct app *a) {
    for (int r = 0; r < APP_ROWS; r++)
        for (int c = 0; c < APP_COLS; c++) { a->grid[r][c] = ' '; a->gcol[r][c] = 0; }
    a->cx = a->cy = 0;
    a->sb_count = 0; a->view = 0;
    a->gdirty = 1;
}
static void grid_scroll(struct app *a) {
    /* the top line is about to scroll off — keep it in the scrollback ring */
    if (a->sb_count < SB_ROWS) {
        memcpy(a->sb[a->sb_count++], a->grid[0], APP_COLS);
        if (a->view > 0 && a->view < a->sb_count) a->view++;   /* stay on the same lines */
    } else {
        for (int r = 1; r < SB_ROWS; r++) memcpy(a->sb[r-1], a->sb[r], APP_COLS);
        memcpy(a->sb[SB_ROWS-1], a->grid[0], APP_COLS);
    }
    for (int r = 1; r < APP_ROWS; r++) memcpy(a->grid[r-1], a->grid[r], APP_COLS);
    for (int c = 0; c < APP_COLS; c++) a->grid[APP_ROWS-1][c] = ' ';
    for (int r = 1; r < APP_ROWS; r++) memcpy(a->gcol[r-1], a->gcol[r], APP_COLS);   /* live colours scroll with their rows */
    for (int c = 0; c < APP_COLS; c++) a->gcol[APP_ROWS-1][c] = 0;
    a->cy = APP_ROWS - 1;
}
static void grid_nl(struct app *a) { a->cx = 0; if (++a->cy >= APP_ROWS) grid_scroll(a); }
/* Erase up to `n` echoed chars, but never past the input start (cx0,cy0) — so
 * history recall can't blank the prompt or earlier output. Handles wrapping. */
static void grid_erase(struct app *a, int n, int cx0, int cy0) {
    int avail = (a->cy - cy0) * APP_COLS + (a->cx - cx0);
    if (n > avail) n = avail;
    for (int i = 0; i < n; i++) {
        if (a->cx > 0) a->cx--;
        else if (a->cy > 0) { a->cy--; a->cx = APP_COLS - 1; }
        a->grid[a->cy][a->cx] = ' ';
    }
}
static void grid_putc(struct app *a, char ch) {
    a->gdirty = 1;
    if (ch == '\n') { grid_nl(a); return; }
    if (ch == '\r') { a->cx = 0; return; }
    a->grid[a->cy][a->cx] = ch;
    a->gcol[a->cy][a->cx] = a->curcol;
    if (++a->cx >= APP_COLS) grid_nl(a);
}

/* Move the echo cursor over already-painted cells (no clearing), for in-line
 * editing (left/right/home/end). Wrapping mirrors grid_putc/grid_erase. */
static void cursor_back(struct app *a, int k) {
    while (k-- > 0) {
        if (a->cx > 0) a->cx--;
        else if (a->cy > 0) { a->cy--; a->cx = APP_COLS - 1; }
    }
    a->gdirty = 1;
}
static void cursor_fwd(struct app *a, int k) {
    while (k-- > 0)
        if (++a->cx >= APP_COLS) { a->cx = 0; if (++a->cy >= APP_ROWS) grid_scroll(a); }
    a->gdirty = 1;
}
static void emit_range(struct app *a, const char *buf, unsigned i, unsigned j) {
    for (; i < j; i++) grid_putc(a, buf[i]);
}

/* ---- system clipboard (one buffer shared by every app) -------------------
 * Set by a terminal text selection, read by middle-click paste — so text can
 * be carried between windows (e.g. a URL from the browser into the shell). */
static char g_clip[CLIP_MAX];
static int  g_clip_len;
void clip_set(const char *s, int n) {
    if (n < 0) n = 0;
    if (n > CLIP_MAX - 1) n = CLIP_MAX - 1;
    for (int i = 0; i < n; i++) g_clip[i] = s[i];
    g_clip[n] = 0; g_clip_len = n;
}
int clip_get(char *out, int max) {
    int n = g_clip_len;
    if (n > max - 1) n = max - 1;
    for (int i = 0; i < n; i++) out[i] = g_clip[i];
    if (max > 0) out[n] = 0;
    return n;
}

/* The character currently displayed at visible row r, column c — reading from
 * the scrollback or the live grid exactly as app_render does, so selection
 * highlighting and text extraction match what's on screen. */
static char app_cell(struct app *a, int r, int c) {
    if (r < 0 || r >= APP_ROWS || c < 0 || c >= APP_COLS) return ' ';
    int L = (a->sb_count - a->view) + r;
    if (L >= 0 && L < a->sb_count) return a->sb[L][c];
    if (L >= a->sb_count && (L - a->sb_count) < APP_ROWS) return a->grid[L - a->sb_count][c];
    return ' ';
}

/* Order the selection so (r0,c0) is the top-left end and (r1,c1) the bottom-right. */
static void sel_ordered(struct app *a, int *r0, int *c0, int *r1, int *c1) {
    *r0 = a->sel_r0; *c0 = a->sel_c0; *r1 = a->sel_r1; *c1 = a->sel_c1;
    if (*r1 < *r0 || (*r1 == *r0 && *c1 < *c0)) {
        int tr = *r0, tc = *c0; *r0 = *r1; *c0 = *c1; *r1 = tr; *c1 = tc;
    }
}

void app_render(app_t *a, int px, int py, int focused) {
    /* Show a 17-row window into [scrollback ... live grid], scrolled up by view. */
    for (int r = 0; r < APP_ROWS; r++) {
        int L = (a->sb_count - a->view) + r;        /* logical row in the combined buffer */
        for (int c = 0; c < APP_COLS; c++) {
            char ch = ' '; uint32_t fg = 0x33FF66;
            if (L >= 0 && L < a->sb_count) ch = a->sb[L][c];               /* scrollback: default green */
            else if (L >= a->sb_count && (L - a->sb_count) < APP_ROWS) {
                int gr = L - a->sb_count; ch = a->grid[gr][c];
                fg = app_palette[a->gcol[gr][c] & 15];                     /* live grid: per-cell colour */
            }
            fb_glyph(px + c * font_width, py + r * font_height, ch, fg, 0x0A0A0A);
        }
    }
    /* Scrollback scrollbar on the right edge (only when there's scrollback): a
     * dark track with a thumb whose size = visible/total and whose position
     * tracks `view`. Gives the wheel/PgUp scrollback visible feedback. */
    if (a->sb_count > 0) {
        int total = a->sb_count + APP_ROWS;
        int sbx = px + APP_COLS * font_width + 1;
        int trackh = APP_ROWS * font_height;
        fb_fill_rect(sbx, py, 3, trackh, 0x202428);
        int th = trackh * APP_ROWS / total; if (th < 8) th = 8;
        int top = a->sb_count - a->view;                       /* first visible logical row */
        int ty = py + (trackh - th) * top / (a->sb_count ? a->sb_count : 1);
        fb_fill_rect(sbx, ty, 3, th, 0x6A7480);
    } else if (a->view > 0)                          /* (fallback) scrolled-up indicator */
        fb_glyph(px + (APP_COLS - 1) * font_width, py, '^', 0xFFD060, 0x0A0A0A);
    /* Text selection highlight (white on blue), drawn over the cells. Linear,
     * line-spanning: the first row runs from c0, the last to c1, rows between
     * are full-width — matching app_sel_commit's extraction. */
    if (a->sel_on) {
        int r0, c0, r1, c1; sel_ordered(a, &r0, &c0, &r1, &c1);
        for (int r = r0; r <= r1 && r < APP_ROWS; r++) {
            if (r < 0) continue;
            int cs = (r == r0) ? c0 : 0, ce = (r == r1) ? c1 : APP_COLS;
            for (int c = cs; c < ce && c < APP_COLS; c++)
                fb_glyph(px + c * font_width, py + r * font_height, app_cell(a, r, c), 0xFFFFFF, 0x2C66D6);
        }
    }
    /* Block caret on the focused window at the live cursor, when it's in view
     * (hidden while scrolled up into the scrollback). Drawn over the cell so it
     * tracks left/right/home/end edits, not just the end of the line. */
    if (focused && !a->gfx && !a->caret_off) {
        int cr = a->cy + a->view;
        if (cr >= 0 && cr < APP_ROWS && a->cx >= 0 && a->cx < APP_COLS) {
            char ch = a->grid[a->cy][a->cx];
            fb_glyph(px + a->cx * font_width, py + cr * font_height,
                     (ch && ch != ' ') ? ch : ' ', 0x0A0A0A, 0x33FF66);
        }
    }
}

int app_alive(app_t *a) { return a && a->used && !a->exited; }

/* Reclaim a self-exited app's resources. Called by the window manager from ITS
 * own context (not the app's), so it can free the app's task_t + 256 KB kernel
 * stack and release the apps[] slot — lifting the per-boot spawn cap for apps
 * that exit cleanly. Only acts once the task is fully dead (off-CPU; see
 * task_free), so a still-running task is never freed under it. Returns 1 when
 * the slot is free (the WM may then drop the window), 0 if the app exited but
 * its task isn't off-CPU yet (retry next pass). Frees the task_t + kernel stack,
 * the app's address space (a->cr3 — page tables + user frames, via
 * vmm_destroy_address_space), and the apps[] slot. */
int app_reap(app_t *a) {
    if (!a) return 1;
    if (a->used && a->exited && (!a->task || a->task->state == TASK_DEAD)) {
        if (a->task) task_free(a->task);
        a->task = 0;
        vmm_destroy_address_space(a->cr3);   /* free page tables + user frames */
        a->cr3 = 0;
        if (a->gfx) { kfree(a->gfx); a->gfx = 0; }   /* graphics canvas (kernel heap) */
        a->used = 0;
    }
    return !a->used;
}

/* Ask a running app to close (e.g. the user clicked the window's X or pressed
 * the close key). We can't safely free a running task from outside, so instead
 * we raise a flag and wake the app: it next returns from its blocking input
 * read, sees the flag, and calls task_exit() from its OWN context — a clean
 * exit. The WM then reaps it (app_reap) like any other exited app. Apps that
 * are busy (not waiting for input) close once they next read input. */
void app_request_kill(app_t *a) {
    if (a && a->used && !a->exited) {
        a->kill = 1;
        task_wake(a->task);   /* unblock it if it's sleeping in app_sys_read */
    }
}

/* Honor a pending WM kill from a NON-blocking per-frame syscall (pollkey /
 * gfx_blit / sleep). A polling or graphics app never calls the blocking
 * app_sys_read where the kill is otherwise observed, so without this F8 / the
 * window [x] / the context-menu Close couldn't close it (it'd only exit on its
 * own quit key). Exit cleanly — the WM then reaps it — exactly as app_sys_read
 * does on a->kill. Called every frame, so the close lands within ~one frame. */
void app_kill_check(void) {
    struct app *a = cur();
    if (a && a->kill) { a->exited = 1; task_exit(); }
}

/* WM polls this: returns 1 (and clears) if the app's grid changed since asked. */
int app_dirty_clear(app_t *a) { int d = a->gdirty; a->gdirty = 0; return d; }

/* ---- input queue (filled by the WM, drained by SYS_read) ---- */
#define SIGINT 2
void app_key(app_t *a, char c) {
    /* Ctrl-C (0x83): if this app installed a SIGINT handler, raise it asynchronously
     * (interrupting even a runaway compute loop) instead of queueing the key. Opt-in,
     * so the shell — which polls 0x83 to break its own loops — is unaffected. M1083. */
    if ((unsigned char)c == 0x83 && a->sig_handler[SIGINT]) { app_request_signal(a, SIGINT); return; }
    /* PgUp/PgDn scroll the scrollback for ordinary terminals; a full-screen app
     * that draws its own view (caret_off, e.g. the editor) gets them as keys to
     * page its own content instead. */
    if (c == 0x15 && !a->caret_off) {        /* PgUp: scroll into the scrollback history */
        if (a->view < a->sb_count) { a->view += 4; if (a->view > a->sb_count) a->view = a->sb_count; a->gdirty = 1; }
        return;                  /* a UI control — the program never sees it */
    }
    if (c == 0x16 && !a->caret_off) {        /* PgDn: scroll back toward the live bottom */
        if (a->view > 0) { a->view -= 4; if (a->view < 0) a->view = 0; a->gdirty = 1; }
        return;
    }
    if (a->view != 0) { a->view = 0; a->gdirty = 1; }   /* typing returns to the live view */
    int n = (a->ih + 1) % IQ_SIZE;
    if (n != a->it) { a->iq[a->ih] = c; a->ih = n; }
    task_wake(a->task);          /* unblock the app if it's waiting in read() */
}
static int iq_get(struct app *a) {
    if (a->paste_pos < a->paste_len)            /* drain a pending paste first (not capped by IQ_SIZE) */
        return (unsigned char)a->pastebuf[a->paste_pos++];
    if (a->ih == a->it) return -1;
    char c = a->iq[a->it]; a->it = (a->it + 1) % IQ_SIZE; return (unsigned char)c;
}

/* Non-blocking: next key for the calling app, or -1 if none (for games). */
int app_sys_pollkey(void) { app_kill_check(); return iq_get(cur()); }

/* Save/disable + restore interrupts, to make "check queue then block" atomic
 * against the window manager delivering a key (closes a lost-wakeup race). */
static inline uint64_t irq_save(void) {
    uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory", "cc");
}

/* ---- syscall-facing ---- */
/* ANSI/VT100: map an SGR colour code (30-37 normal / 90-97 bright) onto our
 * 16-entry app_palette (which isn't in ANSI order). */
static uint8_t ansi_color(int code, int bold) {
    static const uint8_t base[8]   = { 8, 2, 0, 3, 6, 5, 4, 1 };   /* blk red grn yel blu mag cyn wht */
    static const uint8_t bright[8] = { 8, 13, 9, 12, 14, 11, 10, 1 };
    if (code >= 90 && code <= 97) return bright[code - 90];
    if (code >= 30 && code <= 37) return (bold ? bright : base)[code - 30];
    return 0;
}

/* Execute one buffered CSI sequence (a->csi[0..csilen)) ending in `final`. A
 * tiny VT100 subset: SGR colours (m), cursor moves (A/B/C/D/H/f), erase (J/K). */
static void ansi_csi(struct app *a, char final) {
    int p[8] = {0}, np = 0, cur = 0;
    for (int i = 0; i < a->csilen; i++) {
        char c = a->csi[i];
        if (c >= '0' && c <= '9') cur = cur * 10 + (c - '0');
        else if (c == ';') { if (np < 7) p[np++] = cur; cur = 0; }
    }
    p[np++] = cur;                       /* the last/only param; np >= 1 */
    int n = p[0] ? p[0] : 1;             /* default-1 count for cursor moves */
    switch (final) {
    case 'm': {                          /* SGR: text colour */
        int bold = 0;
        for (int i = 0; i < np; i++) {
            int v = p[i];
            if (v == 0) { a->curcol = 0; bold = 0; }
            else if (v == 1) bold = 1;
            else if (v == 39) a->curcol = 0;
            else if ((v >= 30 && v <= 37) || (v >= 90 && v <= 97)) a->curcol = ansi_color(v, bold);
        }
        break;
    }
    case 'A': a->cy -= n; if (a->cy < 0) a->cy = 0; break;
    case 'B': a->cy += n; if (a->cy >= APP_ROWS) a->cy = APP_ROWS - 1; break;
    case 'C': a->cx += n; if (a->cx >= APP_COLS) a->cx = APP_COLS - 1; break;
    case 'D': a->cx -= n; if (a->cx < 0) a->cx = 0; break;
    case 'H': case 'f': {                /* cursor to row;col (1-based) */
        int row = p[0] ? p[0] : 1, col = (np >= 2 && p[1]) ? p[1] : 1;
        a->cy = row - 1; a->cx = col - 1;
        if (a->cy < 0) a->cy = 0; if (a->cy >= APP_ROWS) a->cy = APP_ROWS - 1;
        if (a->cx < 0) a->cx = 0; if (a->cx >= APP_COLS) a->cx = APP_COLS - 1;
        break;
    }
    case 'J': {                          /* erase in display (2 = whole screen) */
        int m = p[0];
        int y0 = (m == 2) ? 0 : a->cy;
        if (m == 2) { a->cx = a->cy = 0; }
        for (int x = (m == 2 ? 0 : a->cx); x < APP_COLS; x++) { a->grid[y0][x] = ' '; a->gcol[y0][x] = 0; }
        for (int y = y0 + 1; y < APP_ROWS; y++)
            for (int x = 0; x < APP_COLS; x++) { a->grid[y][x] = ' '; a->gcol[y][x] = 0; }
        break;
    }
    case 'K': {                          /* erase in line (0 to-eol, 1 from-bol, 2 whole) */
        int m = p[0];
        int x0 = (m == 1 || m == 2) ? 0 : a->cx;
        int x1 = (m == 1) ? a->cx + 1 : APP_COLS;
        for (int x = x0; x < x1 && x < APP_COLS; x++) { a->grid[a->cy][x] = ' '; a->gcol[a->cy][x] = 0; }
        break;
    }
    }
    a->gdirty = 1;
}

/* App stdout. Bytes pass straight to the grid EXCEPT ANSI escape sequences
 * (ESC [ ... <letter>), which are parsed for colour/cursor/erase. Output with
 * no ESC byte renders byte-identically to before, so existing apps are
 * unaffected. */
void app_sys_write(const char *buf, unsigned len) {
    struct app *a = cur();
    for (unsigned i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (a->esc == 0) {
            if (ch == 0x1B) a->esc = 1;          /* ESC: maybe a sequence */
            else grid_putc(a, (char)ch);
        } else if (a->esc == 1) {                /* after ESC */
            if (ch == '[') { a->esc = 2; a->csilen = 0; }
            else a->esc = 0;                     /* unsupported ESC x: consume + drop */
        } else {                                 /* in CSI: collect until the final byte */
            if (ch >= 0x40 && ch <= 0x7E) { ansi_csi(a, (char)ch); a->esc = 0; }
            else if (a->csilen < sizeof(a->csi)) a->csi[a->csilen++] = (char)ch;
            else a->esc = 0;                     /* overlong: bail (no runaway) */
        }
    }
}

/* Replace the on-screen line with history entry `idx` (or empty); returns len. */
static unsigned hist_recall(struct app *a, char *buf, unsigned max, unsigned cur_n,
                            int idx, int cx0, int cy0) {
    grid_erase(a, (int)cur_n, cx0, cy0);
    unsigned n = 0;
    if (idx >= 0 && idx < a->hist_n) {
        const char *h = a->hist[idx];
        while (h[n] && n < max - 1) { grid_putc(a, h[n]); buf[n] = h[n]; n++; }
    }
    return n;
}

int app_sys_read(char *buf, unsigned max) {
    struct app *a = cur();
    unsigned n = 0;                                 /* line length          */
    unsigned cur_i = 0;                             /* caret index in [0,n] */
    int cx0 = a->cx, cy0 = a->cy;                   /* where the input starts */
    a->hist_pos = a->hist_n;                        /* start just past the newest */
    while (n < max) {
        uint64_t f = irq_save();                    /* check kill + queue + block ATOMICALLY: if the kill check sat outside this region, a kill+task_wake from the WM landing between the check and task_block() would be lost (the wake no-ops on a not-yet-blocked task) -> the app sleeps forever and its window is never reaped */
        if (a->kill) { irq_restore(f); a->exited = 1; task_exit(); }  /* WM asked us to close: exit cleanly (WM then reaps) */
        int c = iq_get(a);
        if (c < 0) { task_block(); irq_restore(f); continue; }  /* sleep until woken (incl. by a kill request) */
        irq_restore(f);
        /* Ctrl+letter arrives as 0x81..0x9A. Map the readline navigation aliases
         * onto the existing key codes; the kill/cancel ones are handled below. */
        switch (c) {
            case 0x81: c = 0x01; break;   /* Ctrl-A -> Home   */
            case 0x85: c = 0x05; break;   /* Ctrl-E -> End    */
            case 0x82: c = 0x13; break;   /* Ctrl-B -> left   */
            case 0x86: c = 0x14; break;   /* Ctrl-F -> right  */
            case 0x90: c = 0x11; break;   /* Ctrl-P -> prev (history up)   */
            case 0x8e: c = 0x12; break;   /* Ctrl-N -> next (history down) */
            case 0x84: c = 0x04; break;   /* Ctrl-D -> Delete */
            case 0x88: c = 0x08; break;   /* Ctrl-H -> backspace */
        }
        if (c == 0x83) {                  /* Ctrl-C: abandon the current line */
            cursor_fwd(a, (int)(n - cur_i));
            grid_putc(a, '^'); grid_putc(a, 'C'); grid_putc(a, '\n');
            buf[0] = 0; return 0;
        }
        if (c == 0x8b || c == 0x95 || c == 0x97) {   /* Ctrl-K / Ctrl-U / Ctrl-W: kill operations */
            unsigned oldlen = n, oldcur = cur_i;
            if (c == 0x8b) { n = cur_i; }                          /* kill to end of line */
            else if (c == 0x95) {                                  /* kill the whole line */
                for (unsigned k = cur_i; k < n; k++) buf[k - cur_i] = buf[k];
                n -= cur_i; cur_i = 0;
            } else {                                               /* Ctrl-W: kill the word before the caret */
                unsigned ws = cur_i;
                while (ws > 0 && buf[ws-1] == ' ') ws--;
                while (ws > 0 && buf[ws-1] != ' ') ws--;
                unsigned del = cur_i - ws;
                for (unsigned k = cur_i; k < n; k++) buf[k - del] = buf[k];
                n -= del; cur_i = ws;
            }
            cursor_back(a, (int)oldcur);                           /* repaint: start -> content -> blank tail */
            emit_range(a, buf, 0, n);
            for (unsigned k = n; k < oldlen; k++) grid_putc(a, ' ');
            cursor_back(a, (int)((oldlen > n ? oldlen : n) - cur_i));
            continue;
        }
        if (c == 0x8c) {                  /* Ctrl-L: clear the screen, keep the current line at the top */
            int oy = cy0, span = a->cy - cy0; if (span < 0) span = 0;
            for (int r = 0; r <= span && oy + r < APP_ROWS; r++)   /* move the prompt+input rows up */
                for (int col = 0; col < APP_COLS; col++) {
                    a->grid[r][col] = a->grid[oy + r][col];
                    a->gcol[r][col] = a->gcol[oy + r][col];
                }
            for (int r = span + 1; r < APP_ROWS; r++)              /* blank everything below */
                for (int col = 0; col < APP_COLS; col++) { a->grid[r][col] = ' '; a->gcol[r][col] = 0; }
            a->cy -= oy; cy0 = 0; a->view = 0; a->gdirty = 1;
            continue;
        }
        if (c >= 0x80) continue;          /* any other Ctrl-combo: ignore (don't echo) */
        if (c == '\n' || c == '\r') {
            cursor_fwd(a, (int)(n - cur_i)); cur_i = n;  /* commit from end of line */
            if (n > 0) {                            /* save this line to history */
                int len = n < 95 ? (int)n : 95, slot;
                if (a->hist_n < HIST_N) slot = a->hist_n++;
                else { for (int k = 1; k < HIST_N; k++) memcpy(a->hist[k-1], a->hist[k], 96); slot = HIST_N - 1; }
                for (int i = 0; i < len; i++) a->hist[slot][i] = buf[i];
                a->hist[slot][len] = 0;
            }
            grid_putc(a, '\n'); buf[n++] = '\n'; break;
        }
        if (c == '\b' || c == 127) {                /* backspace: delete char before caret */
            if (cur_i > 0) {
                for (unsigned i = cur_i; i < n; i++) buf[i-1] = buf[i];
                n--; cur_i--;
                cursor_back(a, 1);
                emit_range(a, buf, cur_i, n); grid_putc(a, ' ');
                cursor_back(a, (int)(n - cur_i) + 1);
            }
            continue;
        }
        if (c == 0x04) {                            /* Delete: delete char at caret */
            if (cur_i < n) {
                for (unsigned i = cur_i + 1; i < n; i++) buf[i-1] = buf[i];
                n--;
                emit_range(a, buf, cur_i, n); grid_putc(a, ' ');
                cursor_back(a, (int)(n - cur_i) + 1);
            }
            continue;
        }
        if (c == 0x13) { if (cur_i > 0) { cursor_back(a, 1); cur_i--; } continue; }       /* left  */
        if (c == 0x14) { if (cur_i < n) { cursor_fwd(a, 1); cur_i++; } continue; }        /* right */
        if (c == 0x01) { if (cur_i > 0) { cursor_back(a, (int)cur_i); cur_i = 0; } continue; }      /* Home */
        if (c == 0x05) { if (cur_i < n) { cursor_fwd(a, (int)(n - cur_i)); cur_i = n; } continue; } /* End  */
        if (c == 0x11) {                            /* up-arrow: older command */
            if (a->hist_pos > 0) {
                cursor_fwd(a, (int)(n - cur_i));
                n = hist_recall(a, buf, max, n, --a->hist_pos, cx0, cy0); cur_i = n;
            }
            continue;
        }
        if (c == 0x12) {                            /* down-arrow: newer command */
            if (a->hist_pos < a->hist_n) {
                cursor_fwd(a, (int)(n - cur_i));
                n = hist_recall(a, buf, max, n, ++a->hist_pos, cx0, cy0); cur_i = n;
            }
            continue;
        }
        if (c == '\t') {                            /* Tab: complete a filename from the cwd */
            cursor_fwd(a, (int)(n - cur_i)); cur_i = n;   /* completion acts at end of line */
            int ws = (int)n; while (ws > 0 && buf[ws-1] != ' ') ws--;
            int plen = (int)n - ws, slash = 0;
            for (int i = ws; i < (int)n; i++) if (buf[i] == '/') slash = 1;
            /* Complete when there is a word to extend (plen>0) or an empty
             * argument after a command (ws>0) — `cmd <Tab>` lists every file,
             * like bash. A wholly empty line (ws==0, plen==0) does nothing. */
            if ((plen > 0 || ws > 0) && !slash) {
                /* Complete to the longest common prefix of every cwd entry whose
                 * name starts with the typed word (case-insensitive). A unique
                 * match fills in the whole name; several matches advance to the
                 * point where they first disagree — bash's default Tab. */
                vfs_dirent e[32]; int ne = vfs_list(e, 32);
                const char *names[32];
                for (int i = 0; i < ne; i++) names[i] = e[i].name;
                int nm, fmi, cpl = complete_scan(names, ne, buf + ws, plen, &nm, &fmi);
                if (nm >= 1 && cpl > plen) {        /* extend the word to the common prefix (canonical case) */
                    grid_erase(a, plen, cx0, cy0); n -= (unsigned)plen;
                    for (int k = 0; k < cpl && n + 1 < max; k++) {
                        grid_putc(a, e[fmi].name[k]); buf[n++] = e[fmi].name[k];
                    }
                    if (nm == 1 && n + 1 < max) {   /* unique: '/' to descend a dir, else a space for the next arg */
                        char tail = e[fmi].name[cpl] == '/' ? '/' : ' ';
                        grid_putc(a, tail); buf[n++] = tail;
                    }
                } else if (nm > 1) {               /* already at the common prefix: list the candidates,
                                                    * then redraw the prompt + line (bash's second Tab) */
                    char psave[APP_COLS]; uint8_t csave[APP_COLS];
                    int pn = cx0 < APP_COLS ? cx0 : APP_COLS;
                    for (int k = 0; k < pn; k++) { psave[k] = a->grid[cy0][k]; csave[k] = a->gcol[cy0][k]; }
                    grid_putc(a, '\n');
                    for (int i = 0; i < ne; i++) {
                        if (!complete_match(e[i].name, buf + ws, plen)) continue;
                        for (int k = 0; e[i].name[k]; k++) grid_putc(a, e[i].name[k]);
                        grid_putc(a, ' '); grid_putc(a, ' ');
                    }
                    grid_putc(a, '\n');
                    uint8_t savecol = a->curcol;   /* repaint the prompt in its original colours */
                    for (int k = 0; k < pn; k++) { a->curcol = csave[k]; grid_putc(a, psave[k]); }
                    a->curcol = savecol;
                    cx0 = a->cx; cy0 = a->cy;       /* input restarts after the redrawn prompt */
                    emit_range(a, buf, 0, n);
                }
            }
            cur_i = n;
            continue;
        }
        if (c < 32) continue;                       /* other control keys: ignore */
        if (n + 1 < max) {                          /* printable: insert at the caret */
            for (unsigned i = n; i > cur_i; i--) buf[i] = buf[i-1];
            buf[cur_i] = (char)c; n++;
            emit_range(a, buf, cur_i, n); cur_i++;
            cursor_back(a, (int)(n - cur_i));       /* park caret just after the new char */
        }
    }
    return (int)n;
}

/* SYS_sbrk: grow the calling app's heap by `inc` bytes (rounded up to whole
 * pages), mapping fresh USER|WRITABLE frames into its address space, and return
 * the PREVIOUS break so ulib's malloc can carve allocations from [old, new).
 * Returns (uint64_t)-1 on out-of-memory or if the heap would reach the stack.
 * We only ever grow: a non-positive inc just reports the current break, since
 * ulib's allocator reuses freed space itself (the kernel never has to shrink).
 * Frames mapped here are reclaimed wholesale by vmm_destroy_address_space when
 * the app exits, so even the OOM path below leaks nothing past the app's life. */
uint64_t app_sbrk(long inc) {
    struct app *a = cur();
    if (!a) return (uint64_t)-1;
    if (!a->heap_end) a->heap_end = UHEAP_BASE;       /* lazily start at the heap base */
    uint64_t old = a->heap_end;
    if (inc <= 0) return old;
    uint64_t pages  = ((uint64_t)inc + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t newend = old + pages * PAGE_SIZE;
    if (newend > UHEAP_LIMIT || newend < old) return (uint64_t)-1;   /* hit the stack / overflow */
    for (uint64_t v = old; v < newend; v += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {                                 /* OOM: undo this call's partial mapping */
            for (uint64_t u = old; u < v; u += PAGE_SIZE) {
                uint64_t ph = vmm_translate(u);
                vmm_unmap(u);
                if (ph) pmm_free_frame(ph);
            }
            /* Make memory exhaustion visible instead of a silent app death — this
             * is the path Quake hit at 128 MB (M599). The app's malloc gets NULL
             * next and usually exits, so this logs about once, not in a spin. */
            kprintf("[app] '%s' out of memory: sbrk(%ld) failed, no free frames (increase QEMU -m?)\n",
                    a->title ? a->title : "?", inc);
            return (uint64_t)-1;
        }
        vmm_map(v, frame, PTE_WRITABLE | PTE_USER | PTE_NX);   /* heap: data, never code (W^X) */
    }
    a->heap_end = newend;
    return old;
}

/* --- mmap: demand-paged anonymous memory (M1063) ---------------------------
 * SYS_mmap reserves a region in a private VA window; its pages are NOT mapped
 * up front — the first touch of each page faults, and app_fault_handle (called
 * from the #PF handler) lazily allocates + maps a zeroed frame. This is the
 * core demand-paging mechanism, and the seed for file-backed mmap + COW/fork. */
#define MMAP_BASE  0x60000000ull        /* above the 0x50000000 user stack, clear of the heap */
#define MMAP_TOP   0x70000000ull

uint64_t app_mmap(uint64_t len) {
    struct app *a = cur();
    if (!a || len == 0) return 0;
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (a->nvma >= APP_MAXVMA) return 0;
    if (a->mmap_next < MMAP_BASE) a->mmap_next = MMAP_BASE;
    uint64_t addr = a->mmap_next;
    if (addr + len > MMAP_TOP || addr + len < addr) return 0;
    a->vma[a->nvma].start = addr;
    a->vma[a->nvma].len   = len;
    a->nvma++;
    a->mmap_next = addr + len + PAGE_SIZE;          /* leave an unmapped guard gap */
    return addr;
}

int app_munmap(uint64_t addr, uint64_t len) {
    struct app *a = cur();
    if (!a) return -1;
    (void)len;
    for (int i = 0; i < a->nvma; i++) {
        if (a->vma[i].start == addr) {
            for (uint64_t p = a->vma[i].start; p < a->vma[i].start + a->vma[i].len; p += PAGE_SIZE) {
                uint64_t ph = vmm_translate(p);
                if (ph) { vmm_unmap(p); pmm_free_frame(ph); }
            }
            a->vma[i] = a->vma[a->nvma - 1];
            a->nvma--;
            return 0;
        }
    }
    return -1;
}

/* mprotect (M1090): change the R/W/X protection of an already-mapped range in
 * the calling app (PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4). Enables W^X and
 * write-then-execute JIT pages. The range must be the app's own user pages. */
int app_mprotect(uint64_t addr, uint64_t len, int prot) {
    if (len == 0) return -1;
    uint64_t a0 = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = (addr + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (end <= a0 || !vmm_user_ok(a0, end - a0)) return -1;   /* must be the caller's mapped user pages */
    uint64_t flags = PTE_USER;
    if (prot & 0x2) flags |= PTE_WRITABLE;       /* PROT_WRITE  */
    if (!(prot & 0x4)) flags |= PTE_NX;          /* not PROT_EXEC -> no-execute */
    for (uint64_t p = a0; p < end; p += PAGE_SIZE)
        if (vmm_protect(p, flags) < 0) return -1;
    return 0;
}

/* Magic (mirrored) ring buffer (M1089): reserve `len` bytes of physical frames
 * and map them TWICE, back to back, so the region [base, base+2*len) has its
 * second half alias the first. A wraparound queue then needs no split-handling
 * or modulo — a read/write that crosses base+len continues seamlessly into the
 * same frames. Mapped eagerly (no demand faults); each frame is pmm_addref'd for
 * its second mapping so exit/munmap (which frees every PTE's frame) releases it
 * exactly once. Returns the base VA, or 0. */
uint64_t app_ringbuf(uint64_t len) {
    struct app *a = cur();
    if (!a || len == 0) return 0;
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t total = len * 2;
    if (total < len) return 0;                       /* overflow */
    if (a->nvma >= APP_MAXVMA) return 0;
    if (a->mmap_next < MMAP_BASE) a->mmap_next = MMAP_BASE;
    uint64_t base = a->mmap_next;
    if (base + total > MMAP_TOP || base + total < base) return 0;
    uint64_t mapped = 0;
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {                                /* OOM: unwind what we mapped */
            for (uint64_t u = 0; u < mapped; u += PAGE_SIZE) {
                uint64_t ph = vmm_translate(base + u);
                vmm_unmap(base + len + u); vmm_unmap(base + u);
                if (ph) pmm_free_frame(ph);          /* drops the addref, then frees */
            }
            return 0;
        }
        uint8_t *z = (uint8_t *)hhdm(frame);
        for (int b = 0; b < PAGE_SIZE; b++) z[b] = 0;
        vmm_map(base + off, frame, PTE_WRITABLE | PTE_USER | PTE_NX);          /* primary */
        pmm_addref(frame);
        vmm_map(base + len + off, frame, PTE_WRITABLE | PTE_USER | PTE_NX);    /* mirror */
        __asm__ volatile("invlpg (%0)" : : "r"(base + off) : "memory");
        __asm__ volatile("invlpg (%0)" : : "r"(base + len + off) : "memory");
        mapped += PAGE_SIZE;
    }
    a->vma[a->nvma].start = base;
    a->vma[a->nvma].len   = total;
    a->nvma++;
    a->mmap_next = base + total + PAGE_SIZE;
    return base;
}

/* #PF hook (called from the ring-3 path of the page-fault handler). If `cr2` is
 * inside a reserved mmap region, map a fresh zeroed frame into the (active) app
 * space and report it resolved so the instruction retries; else 0 = a real
 * fault, and the app is terminated as before. */
int app_fault_handle(uint64_t cr2) {
    struct app *a = cur();
    if (!a) return 0;
    for (int i = 0; i < a->nvma; i++) {
        if (cr2 >= a->vma[i].start && cr2 < a->vma[i].start + a->vma[i].len) {
            uint64_t page = cr2 & ~(uint64_t)(PAGE_SIZE - 1);
            if (vmm_translate(page)) return 1;          /* already mapped (race) -> retry */
            uint64_t frame = pmm_alloc_frame();
            if (!frame) return 0;                       /* OOM -> let it fault/die */
            uint8_t *z = (uint8_t *)hhdm(frame);
            for (int b = 0; b < PAGE_SIZE; b++) z[b] = 0; /* never leak stale RAM to userspace */
            vmm_map(page, frame, PTE_WRITABLE | PTE_USER | PTE_NX);
            __asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
            return 1;
        }
    }
    return 0;
}

/* --- ring-3 signals (M1067) ------------------------------------------------
 * A registered handler runs on the app's own user stack; the interrupted
 * context is saved kernel-side (sig_saved), so the user stack only needs the
 * trampoline return address. When the handler returns it falls into the ulib
 * trampoline, which calls SYS_sigreturn to restore the saved context. No
 * nesting (sig_in guards). Dormant unless an app registers a handler. */
void app_signal_set(int signo, uint64_t handler, uint64_t restorer) {
    struct app *a = cur();
    if (!a || signo <= 0 || signo >= APP_NSIG) return;
    a->sig_handler[signo] = handler;
    if (restorer) a->sig_restorer = restorer;
}

int app_signal_deliver(struct registers *r, int signo) {
    struct app *a = cur();
    if (!a || signo <= 0 || signo >= APP_NSIG) return 0;
    if (!a->sig_handler[signo] || !a->sig_restorer || a->sig_in) return 0;
    uint64_t nrsp = ((r->rsp - 128) & ~15ull) - 8;   /* skip red zone, 16-align, room for ret addr */
    if (!vmm_user_ok(nrsp, 8)) return 0;             /* bad user stack -> don't deliver */
    a->sig_saved = *r;                               /* save the interrupted context */
    a->sig_in = 1;
    *(volatile uint64_t *)nrsp = a->sig_restorer;    /* handler's return address -> trampoline */
    r->rsp = nrsp;
    r->rip = a->sig_handler[signo];
    r->rdi = (uint64_t)signo;                        /* handler(int signo) */
    return 1;
}

void app_sigreturn(struct registers *r) {
    struct app *a = cur();
    if (a && a->sig_in) { *r = a->sig_saved; a->sig_in = 0; }   /* resume the interrupted context */
}

/* Raise a signal ASYNCHRONOUSLY on app `a` (e.g. the WM mapping Ctrl-C on the
 * focused window to SIGINT). Opt-in: only if the app installed a handler for it
 * — otherwise we leave the keystroke alone, so the shell's existing 0x83 loop-
 * break and every non-handling app are unaffected. The pending signal is
 * delivered when the app next returns to ring 3 (app_deliver_pending). M1083. */
void app_request_signal(app_t *a, int signo) {
    struct app *ap = (struct app *)a;
    if (!ap || signo <= 0 || signo >= APP_NSIG) return;
    if (!ap->sig_handler[signo]) return;     /* no handler installed -> not opted in */
    ap->pending_sig = signo;
    task_wake(ap->task);                     /* unblock it if it's parked in read() */
}

/* If the app this trap returns to has an async signal pending AND we're heading
 * back to ring-3 code (never mid-syscall), deliver it now. Called from the
 * syscall return and the IRQ tail. Returns 1 if a handler was entered. M1083. */
/* Arm a jail for the very next app_spawn (M1088): the child starts pledged to
 * `promises` and, if `path` is non-empty, unveil-confined to that prefix (rw) —
 * a parent-enforced sandbox the child can't escape (pledge only shrinks). */
void app_jail_next(uint32_t promises, const char *path) {
    g_jail_promises = promises;
    int i = 0; if (path) while (path[i] && i < 63) { g_jail_path[i] = path[i]; i++; }
    g_jail_path[i] = 0;
    g_pend_jail = 1;
}

/* strace (M1084): toggle/read whether an app's syscalls are logged to dmesg. */
void app_set_traced(app_t *a, int on) { struct app *ap = (struct app *)a; if (ap) ap->traced = on ? 1 : 0; }
int  app_is_traced(app_t *a)          { struct app *ap = (struct app *)a; return ap ? ap->traced : 0; }

int app_deliver_pending(struct registers *r) {
    task_t *t = task_self();                 /* called from the IRQ tail on EVERY irq, incl. before
                                              * sched_init (current==NULL) and on kernel tasks -> guard */
    if (!t || !t->proc) return 0;
    struct app *a = (struct app *)t->proc;
    if (!a->pending_sig) return 0;
    if ((r->cs & 3) != 3) return 0;          /* resuming kernel code (mid-syscall) -> defer */
    int sig = a->pending_sig;
    if (app_signal_deliver(r, sig)) { a->pending_sig = 0; return 1; }
    return 0;                                 /* couldn't deliver yet (already in a handler) -> stay pending */
}

/* ---- graphics mode: a per-app pixel canvas the WM composites --------------
 * An app calls app_gfx_init(w,h) to swap its text grid for a w*h pixel canvas
 * (0x00RRGGBB), draws into a userspace buffer, and app_gfx_blit()s it across.
 * The window manager draws the canvas (sized to fit) instead of the grid. This
 * is what lets a real graphical program — DOOM — render to a window. */
#define GFX_MAX_W 1024
#define GFX_MAX_H 768

int app_gfx_init(int w, int h) {
    struct app *a = cur();
    if (!a || w <= 0 || h <= 0 || w > GFX_MAX_W || h > GFX_MAX_H) return -1;
    if (a->gfx && (a->gfx_w != w || a->gfx_h != h)) { kfree(a->gfx); a->gfx = 0; }
    if (!a->gfx) {
        a->gfx = kmalloc((size_t)w * (size_t)h * 4);
        if (!a->gfx) return -1;
    }
    a->gfx_w = w; a->gfx_h = h;
    for (int i = 0; i < w * h; i++) a->gfx[i] = 0;     /* start black */
    a->gdirty = 1;
    return 0;
}

/* Copy the caller's w*h pixel buffer into the canvas and mark the window dirty.
 * The source lives in the app's address space (CR3 is the app's during the
 * syscall) and is validated to be the app's own user pages before the read —
 * otherwise a forged kernel pointer would have the kernel copy its own memory
 * into the canvas and paint it on screen. The destination is exactly
 * gfx_w*gfx_h*4 (kernel-allocated), so it can't be overrun. 0, or -1. */
int app_gfx_blit(const uint32_t *pixels) {
    app_kill_check();                       /* WM close-request: exit before painting the next frame */
    struct app *a = cur();
    if (!a || !a->gfx) return -1;
    if (!vmm_user_ok((uint64_t)pixels, (uint64_t)a->gfx_w * (uint64_t)a->gfx_h * 4)) return -1;
    memcpy(a->gfx, pixels, (size_t)a->gfx_w * (size_t)a->gfx_h * 4);
    a->gdirty = 1;
    return 0;
}

/* WM: the app's canvas + dims (1 if in graphics mode, else 0). */
int app_gfx_get(app_t *a, uint32_t **buf, int *w, int *h) {
    if (!a || !a->gfx) return 0;
    *buf = a->gfx; *w = a->gfx_w; *h = a->gfx_h;
    return 1;
}

/* ---- raw keyboard mode (games) ----
 * In raw mode the WM routes make/break key events (scancode + pressed/released
 * + extended) to this app instead of, or alongside, the cooked ASCII it still
 * gets. DOOM needs key-down AND key-up for held movement/fire. */
void app_set_rawkb(int on) { struct app *a = cur(); if (a) a->rawkb = on ? 1 : 0; }
/* SYS_caret: a full-screen text app that draws its own cursor (e.g. the editor)
 * opts out of the system block caret so the two don't both show. */
void app_set_caret(int on) { struct app *a = cur(); if (a) a->caret_off = on ? 0 : 1; }
int  app_caret_hidden(app_t *a) { return a && a->caret_off; }   /* WM: full-screen self-drawing app? */
int  app_get_rawkb(app_t *a) { return a && a->rawkb; }

/* ---- text selection + paste (driven by the WM's mouse handling) ---------- */
static int clampc(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }

void app_sel_begin(app_t *a, int row, int col) {
    if (!a) return;
    a->sel_r0 = a->sel_r1 = clampc(row, APP_ROWS - 1);
    a->sel_c0 = a->sel_c1 = clampc(col, APP_COLS);
    a->sel_on = 1; a->gdirty = 1;
}
void app_sel_extend(app_t *a, int row, int col) {
    if (!a || !a->sel_on) return;
    a->sel_r1 = clampc(row, APP_ROWS - 1);
    a->sel_c1 = clampc(col, APP_COLS);
    a->gdirty = 1;
}
void app_sel_clear(app_t *a) { if (a && a->sel_on) { a->sel_on = 0; a->gdirty = 1; } }

/* Scroll so the scrollbar thumb sits at fraction num/den down its track (0 =
 * top = oldest scrollback, den = bottom = live). For click/drag on the bar. */
void app_scroll_frac(app_t *a, int num, int den) {
    if (!a || den <= 0) return;
    if (num < 0) num = 0;
    if (num > den) num = den;
    int v = a->sb_count * (den - num) / den;            /* top of track -> view = sb_count */
    if (v < 0) v = 0;
    if (v > a->sb_count) v = a->sb_count;
    if (v != a->view) { a->view = v; a->gdirty = 1; }
}

/* Double-click: select the whitespace-delimited word at (row,col) and copy it. */
void app_sel_word(app_t *a, int row, int col) {
    if (!a || row < 0 || row >= APP_ROWS) return;
    if (col < 0) col = 0;
    if (col >= APP_COLS) col = APP_COLS - 1;
    if (app_cell(a, row, col) == ' ') { app_sel_clear(a); return; }   /* clicked whitespace */
    int s = col, e = col;
    while (s > 0 && app_cell(a, row, s - 1) != ' ') s--;
    while (e < APP_COLS - 1 && app_cell(a, row, e + 1) != ' ') e++;
    a->sel_r0 = a->sel_r1 = row; a->sel_c0 = s; a->sel_c1 = e + 1;
    a->sel_on = 1; a->gdirty = 1;
    app_sel_commit(a);                                  /* -> clipboard */
}

/* Release: extract the selected cells (trailing spaces trimmed per line, rows
 * joined with '\n') into the clipboard. The highlight stays until next input. */
void app_sel_commit(app_t *a) {
    if (!a || !a->sel_on) return;
    if (a->sel_r0 == a->sel_r1 && a->sel_c0 == a->sel_c1) {   /* a plain click, not a drag */
        app_sel_clear(a); return;                             /* clear highlight, keep the clipboard */
    }
    int r0, c0, r1, c1; sel_ordered(a, &r0, &c0, &r1, &c1);
    char buf[CLIP_MAX]; int n = 0;
    for (int r = r0; r <= r1 && r < APP_ROWS && n < CLIP_MAX - 1; r++) {
        if (r < 0) continue;
        int cs = (r == r0) ? c0 : 0, ce = (r == r1) ? c1 : APP_COLS;
        int lineend = n;
        for (int c = cs; c < ce && c < APP_COLS && n < CLIP_MAX - 1; c++) {
            char ch = app_cell(a, r, c);
            buf[n++] = ch;
            if (ch != ' ') lineend = n;          /* remember last non-blank for trimming */
        }
        n = lineend;                              /* trim trailing spaces */
        if (r < r1 && n < CLIP_MAX - 1) buf[n++] = '\n';
    }
    clip_set(buf, n);
}

/* Middle-click paste: feed the clipboard into the app's input queue as if typed
 * (newlines included — for a shell, a multi-line paste runs each line). */
void app_paste(app_t *a) {
    if (!a) return;
    a->paste_len = 0;                                    /* stop any in-flight drain before we refill */
    int n = clip_get(a->pastebuf, sizeof a->pastebuf);   /* fill the app's paste buffer... */
    a->paste_pos = 0;
    a->paste_len = n;                                    /* ...set last so iq_get sees a complete buffer */
    if (a->view) { a->view = 0; a->gdirty = 1; }         /* a paste returns to the live view */
    task_wake(a->task);                                  /* unblock it if waiting in read() */
}

/* WM: deliver one raw key event to a raw-mode app's queue. */
void app_key_raw(app_t *a, unsigned short ev) {
    int n = (a->rqh + 1) % 64;
    if (n != a->rqt) { a->rawiq[a->rqh] = ev; a->rqh = n; }   /* drop on overflow */
}

/* WM: store the cursor position (relative to the gfx canvas; -1,-1 if outside)
 * and button bitmask for an app, each frame, for the focused window. */
void app_set_mouse(app_t *a, int x, int y, int btn) {
    if (!a) return;
    a->ms_x = x; a->ms_y = y; a->ms_btn = btn;
}

/* SYS_mouse: pack the caller's last cursor state — x in bits 0-15 (signed),
 * y in 16-31, buttons in 32-34. ulib unpacks it. */
long app_get_mouse(void) {
    struct app *a = cur();
    if (!a) return 0;
    return ((long)(a->ms_btn & 0x7) << 32)
         | ((long)(a->ms_y & 0xFFFF) << 16)
         | ((long)(a->ms_x & 0xFFFF));
}

/* WM: accumulate relative mouse motion for an app (mouselook). */
void app_add_mouse_rel(app_t *a, int dx, int dy) {
    if (!a) return;
    a->ms_dx += dx; a->ms_dy += dy;
}

/* SYS_mouse_rel: the caller's accumulated relative motion, read + cleared.
 * dx in bits 0-31, dy in bits 32-63 (both signed). */
long app_get_mouse_rel(void) {
    struct app *a = cur();
    if (!a) return 0;
    int dx = a->ms_dx, dy = a->ms_dy;
    a->ms_dx = 0; a->ms_dy = 0;
    return ((long)(uint32_t)dy << 32) | (long)(uint32_t)dx;
}

/* SYS_getkbevent: next raw key event for the caller, or -1 if none (non-blocking). */
int app_sys_getkbevent(void) {
    struct app *a = cur();
    if (!a || a->rqh == a->rqt) return -1;
    unsigned short ev = a->rawiq[a->rqt];
    a->rqt = (a->rqt + 1) % 64;
    return (int)ev;
}

int  app_sys_getpid(void) { return cur()->pid; }
void app_sys_clear(void)  { grid_clear(cur()); }
void app_setcolor(int idx) { struct app *a = cur(); if (a) a->curcol = (uint8_t)(idx & 15); }
void app_sys_exit(void)   { cur()->exited = 1; task_exit(); }
/* A ring-3 task hit a CPU exception (divide error, page fault, …). Mark its app
 * exited so the WM tears down the window, then terminate just this task — the
 * kernel and the rest of the desktop keep running. Does not return. */
void app_fault_current(void) {
    struct app *a = (struct app *)task_self()->proc;
    if (a) a->exited = 1;
    task_exit();
}

/* Format the caller's command history (oldest first) as "  N  command\n"
 * lines into buf. Returns bytes written (excluding the NUL terminator). The
 * history ring is the same one up/down-arrow recall uses (app_sys_read). */
int app_sys_history(char *buf, int max) {
    if (max <= 0) return 0;
    struct app *a = cur();
    int p = 0;
    for (int i = 0; i < a->hist_n; i++) {
        const char *h = a->hist[i];
        char line[112];
        int q = 0;
        line[q++] = ' '; line[q++] = ' ';
        int v = i + 1; char num[4]; int k = 0;
        do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 4);
        while (k) line[q++] = num[--k];
        line[q++] = ' '; line[q++] = ' ';
        for (int j = 0; h[j] && q < (int)sizeof(line) - 1; j++) line[q++] = h[j];
        line[q++] = '\n';
        for (int j = 0; j < q && p + 1 < max; j++) buf[p++] = line[j];
    }
    buf[p < max ? p : max - 1] = 0;
    return p;
}

/* ---- spawn ---- */
static void app_trampoline(void) {
    struct app *a = cur();
    enter_user(a->entry, a->ustack);   /* -> ring 3; returns only via SYS_exit */
}

app_t *app_spawn(const void *elf, const char *title, uint64_t elfsz) {
    struct app *a = 0;
    for (int i = 0; i < MAX_APPS; i++) if (!apps[i].used) { a = &apps[i]; break; }
    if (!a || !elf) return 0;

    memset(a, 0, sizeof(*a));
    a->used = 1;
    a->pid = next_pid++;
    /* copy the title into our own buffer (the caller's string — e.g. a filename
     * from another address space — may not outlive this call). Done here, before
     * the CR3 switch below, while the caller's pointer is still valid. */
    int ti = 0; if (title) while (title[ti] && ti < 23) { a->titlebuf[ti] = title[ti]; ti++; }
    a->titlebuf[ti] = 0;
    a->title = a->titlebuf;
    /* Measured boot (M1096): fold this app's exact ELF image into PCR1 + the
     * event log, in launch order. `elf` is kernel-accessible here (embedded
     * .rodata or a kernel read buffer), before the CR3 switch below. */
    measure_extend(PCR_APPS, elf, elf_image_size(elf, elfsz), a->title);
    if (g_have_pend) {                    /* consume a pending launch arg (one-shot, race-free) */
        int ai = 0; while (g_pend_arg[ai] && ai < 127) { a->launch_arg[ai] = g_pend_arg[ai]; ai++; }
        a->launch_arg[ai] = 0; g_have_pend = 0;
    }
    if (g_pend_jail) {                    /* consume a pending jail: confine the child before it runs (M1088) */
        a->promises = g_jail_promises; a->pledged = 1;
        if (g_jail_path[0]) {
            int pi = 0; while (g_jail_path[pi] && pi < (int)sizeof a->uv[0].path - 1) { a->uv[0].path[pi] = g_jail_path[pi]; pi++; }
            a->uv[0].path[pi] = 0; a->uv[0].perms = UV_R | UV_W; a->nuv = 1; a->uv_active = 1;
        }
        g_pend_jail = 0;
    }
    grid_clear(a);
    a->cr3 = vmm_create_address_space();
    if (!a->cr3) { a->used = 0; return 0; }   /* OOM: no address space — loading CR3=0 would triple-fault */

    /* Load the ELF + user stack into the app's address space. We switch CR3 to
     * it (interrupts off) so the loader's writes land in the right space. */
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    uint64_t old;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old));
    __asm__ volatile("mov %0, %%cr3" : : "r"(a->cr3) : "memory");

    a->entry = elf_load(elf, elfsz);
    if (!a->entry) goto fail_in_space;       /* bad ELF: don't spawn a null task */

    for (int i = 0; i < USTACK_PAGES; i++) {
        uint64_t frame = pmm_alloc_frame();  /* stack: non-executable (W^X) */
        if (!frame) goto fail_in_space;      /* OOM: reclaim the partial space below */
        if (vmm_map(USTACK_BASE + (uint64_t)i * PAGE_SIZE, frame,
                    PTE_WRITABLE | PTE_USER | PTE_NX) != 0) {
            pmm_free_frame(frame);
            goto fail_in_space;
        }
    }
    a->ustack = USTACK_BASE + USTACK_PAGES * PAGE_SIZE;

    __asm__ volatile("mov %0, %%cr3" : : "r"(old) : "memory");
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");

    /* 256 KB kernel stack (vs the 16 KB default): a ring-3 app's syscalls run on
     * its kernel stack, and SYS_https (shell get/wget) runs the TLS handshake +
     * bignum/RSA/ECDSA cert verification there — which overflows a small stack. */
    a->task = task_create_stack(app_trampoline, a->cr3, a, 256 * 1024);
    if (!a->task) {                          /* couldn't create the task (OOM): don't queue a taskless app */
        vmm_destroy_address_space(a->cr3);   /* CR3 already restored to `old` above, so this is safe */
        a->used = 0;
        return 0;
    }

    /* queue it for the window manager to give it a window */
    int n = (pend_h + 1) % MAX_APPS;
    if (n != pend_t) { pending[pend_h] = a; pend_h = n; }
    return a;

fail_in_space:
    /* A failure while the app's CR3 was active (bad ELF, or OOM mapping the
     * stack). Restore the caller's CR3 first, THEN tear down the partial address
     * space — vmm_destroy_address_space refuses to free the active space, and
     * leaving it mapped would leak the PML4/PDPT + every frame elf_load mapped. */
    __asm__ volatile("mov %0, %%cr3" : : "r"(old) : "memory");
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    vmm_destroy_address_space(a->cr3);
    a->used = 0;
    return 0;
}

/* Load and run an ELF program from a FAT32 file (e.g. `run calc.elf`). The ELF
 * bytes are read into a kernel buffer; app_spawn/elf_load copy the segments into
 * the new address space synchronously, so the buffer is freed right after. */
int app_spawn_from_file(const char *path) {
    uint8_t *buf = kmalloc(64 * 1024);          /* our user ELFs are < 18 KB */
    if (!buf) return -1;
    long n = vfs_read(path, buf, 64 * 1024);
    int rc = (n > 0 && app_spawn(buf, path, (uint64_t)n)) ? 0 : -1;  /* title = filename */
    kfree(buf);
    return rc;
}

/* Launch a program by name (used by the Apps menu and the `run` syscall). */
int app_spawn_named(const char *name) {
    for (int i = 0; i < NPROGS; i++) {
        const char *a = progs[i].name, *b = name; int eq = 1;
        while (*a && *b) { if (*a++ != *b++) { eq = 0; break; } }
        if (eq && !*a && !*b) {
            if (app_spawn(progs[i].elf, progs[i].title, ~0ull)) return 0;  /* trusted embedded */
            /* don't fail silently: a failed launch (no free app slot, or the ELF's
             * frames/heap didn't fit) is otherwise invisible — no window, no log.
             * (This is exactly how Quake's out-of-memory failure hid before M599.) */
            kprintf("[app] '%s' failed to launch (no free slot, or out of memory loading it)\n", name);
            return -1;
        }
    }
    kprintf("[app] no such program: '%s'\n", name);
    return -1;
}

/* Launch a registered program with a one-shot launch argument (e.g. a filename
 * the app reads via SYS_getarg). The arg is copied into the new app's struct. */
int app_spawn_named_arg(const char *name, const char *arg) {
    int ai = 0; if (arg) while (arg[ai] && ai < 127) { g_pend_arg[ai] = arg[ai]; ai++; }
    g_pend_arg[ai] = 0; g_have_pend = 1;
    int rc = app_spawn_named(name);
    if (rc < 0) g_have_pend = 0;          /* spawn failed: don't leak the arg to the next app */
    return rc;
}

/* Copy the calling app's launch argument into out (NUL-terminated); returns its
 * length, or 0 if it was launched without one. */
int app_getarg(char *out, int max) {
    struct app *a = cur();
    int n = 0;
    if (a) while (a->launch_arg[n] && n < max - 1) { out[n] = a->launch_arg[n]; n++; }
    if (max > 0) out[n] = 0;
    return n;
}

/* List the registered program names, space-separated, into buf (for the shell's
 * `apps` command). Single source of truth = progs[]. Returns bytes written. */
int app_list_names(char *buf, int max) {
    int n = 0;
    for (int i = 0; i < NPROGS; i++) {
        const char *s = progs[i].name;
        if (i && n + 1 < max) buf[n++] = ' ';
        while (*s && n + 1 < max) buf[n++] = *s++;
    }
    if (max > 0) buf[n] = 0;
    return n;
}

/* The window manager calls this to claim freshly-spawned apps. */
app_t *app_take_pending(void) {
    if (pend_t == pend_h) return 0;
    app_t *a = pending[pend_t];
    pend_t = (pend_t + 1) % MAX_APPS;
    return a;
}

/* pending browse-URL requests (shell `browse <url>` -> WM opens a browser). */
#define MAX_BROWSE 4
static char browse_q[MAX_BROWSE][160];
static int  bq_h, bq_t;

void app_browse(const char *url) {                 /* SYS_browse: queue a URL */
    int n = (bq_h + 1) % MAX_BROWSE;
    if (n == bq_t) return;                          /* full -> drop */
    int i = 0; while (url[i] && i < 159) { browse_q[bq_h][i] = url[i]; i++; }
    browse_q[bq_h][i] = 0;
    bq_h = n;
}
int app_take_browse(char *out, int max) {          /* WM drains; 1 if returned */
    if (bq_t == bq_h) return 0;
    const char *s = browse_q[bq_t]; int i = 0;
    while (s[i] && i < max - 1) { out[i] = s[i]; i++; }
    out[i] = 0;
    bq_t = (bq_t + 1) % MAX_BROWSE;
    return 1;
}
