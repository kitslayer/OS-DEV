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
#include "vmm.h"
#include "pmm.h"
#include "elf.h"
#include "fb.h"
#include "font.h"
#include "string.h"
#include "vfs.h"
#include "kheap.h"
#include <stdint.h>

#define APP_COLS 44
#define APP_ROWS 17
#define SB_ROWS  48          /* scrollback: ~3 screens of history */
#define IQ_SIZE  128
#define MAX_APPS 8

#define USTACK_BASE  0x50000000ull
#define USTACK_PAGES 4

struct app {
    int         used;
    int         pid;
    task_t     *task;
    const char *title;
    char        titlebuf[24];            /* persistent copy of the title */
    uint64_t cr3, entry, ustack;
    char     grid[APP_ROWS][APP_COLS];
    int      cx, cy;
    char     sb[SB_ROWS][APP_COLS];      /* scrollback: lines that scrolled off */
    int      sb_count;                   /* how many scrollback lines are stored */
    int      view;                       /* rows scrolled up from the live bottom */
    char     iq[IQ_SIZE];
    volatile int ih, it;
    volatile int exited;
    char     hist[6][96];                /* recent input lines (for up/down) */
    int      hist_n, hist_pos;
    volatile int gdirty;                 /* grid changed -> WM should repaint */
};

static struct app apps[MAX_APPS];
static int next_pid = 100;

/* apps awaiting a window from the window manager */
static struct app *pending[MAX_APPS];
static int pend_h, pend_t;

/* the embedded programs (see kernel/asm/user_blob.asm) */
extern char shell_elf_start[], clock_elf_start[], calc_elf_start[], snake_elf_start[],
            editor_elf_start[], g2048_elf_start[], life_elf_start[], tetris_elf_start[],
            breakout_elf_start[], mines_elf_start[], sudoku_elf_start[], calendar_elf_start[],
            mandel_elf_start[];
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
    { "mandel", mandel_elf_start, "Mandelbrot" },
};
#define NPROGS (int)(sizeof(progs)/sizeof(progs[0]))

extern void enter_user(uint64_t entry, uint64_t ustack);

int app_cols(void) { return APP_COLS; }
int app_rows(void) { return APP_ROWS; }
const char *app_title(app_t *a) { return a->title; }

static struct app *cur(void) { return (struct app *)task_self()->proc; }

/* ---- text grid ---- */
static void grid_clear(struct app *a) {
    for (int r = 0; r < APP_ROWS; r++)
        for (int c = 0; c < APP_COLS; c++) a->grid[r][c] = ' ';
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
    if (++a->cx >= APP_COLS) grid_nl(a);
}

void app_render(app_t *a, int px, int py) {
    /* Show a 17-row window into [scrollback ... live grid], scrolled up by view. */
    for (int r = 0; r < APP_ROWS; r++) {
        int L = (a->sb_count - a->view) + r;        /* logical row in the combined buffer */
        for (int c = 0; c < APP_COLS; c++) {
            char ch = ' ';
            if (L >= 0 && L < a->sb_count) ch = a->sb[L][c];
            else if (L >= a->sb_count && (L - a->sb_count) < APP_ROWS) ch = a->grid[L - a->sb_count][c];
            fb_glyph(px + c * font_width, py + r * font_height, ch, 0x33FF66, 0x0A0A0A);
        }
    }
    if (a->view > 0)                                /* scrolled-up indicator (top-right) */
        fb_glyph(px + (APP_COLS - 1) * font_width, py, '^', 0xFFD060, 0x0A0A0A);
}

int app_alive(app_t *a) { return a && a->used && !a->exited; }

/* WM polls this: returns 1 (and clears) if the app's grid changed since asked. */
int app_dirty_clear(app_t *a) { int d = a->gdirty; a->gdirty = 0; return d; }

/* ---- input queue (filled by the WM, drained by SYS_read) ---- */
void app_key(app_t *a, char c) {
    if (c == 0x15) {             /* PgUp: scroll into the scrollback history */
        if (a->view < a->sb_count) { a->view += 4; if (a->view > a->sb_count) a->view = a->sb_count; a->gdirty = 1; }
        return;                  /* a UI control — the program never sees it */
    }
    if (c == 0x16) {             /* PgDn: scroll back toward the live bottom */
        if (a->view > 0) { a->view -= 4; if (a->view < 0) a->view = 0; a->gdirty = 1; }
        return;
    }
    if (a->view != 0) { a->view = 0; a->gdirty = 1; }   /* typing returns to the live view */
    int n = (a->ih + 1) % IQ_SIZE;
    if (n != a->it) { a->iq[a->ih] = c; a->ih = n; }
    task_wake(a->task);          /* unblock the app if it's waiting in read() */
}
static int iq_get(struct app *a) {
    if (a->ih == a->it) return -1;
    char c = a->iq[a->it]; a->it = (a->it + 1) % IQ_SIZE; return (unsigned char)c;
}

/* Non-blocking: next key for the calling app, or -1 if none (for games). */
int app_sys_pollkey(void) { return iq_get(cur()); }

/* Save/disable + restore interrupts, to make "check queue then block" atomic
 * against the window manager delivering a key (closes a lost-wakeup race). */
static inline uint64_t irq_save(void) {
    uint64_t f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory", "cc");
}

/* ---- syscall-facing ---- */
void app_sys_write(const char *buf, unsigned len) {
    struct app *a = cur();
    for (unsigned i = 0; i < len; i++) grid_putc(a, buf[i]);
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
    unsigned n = 0;
    int cx0 = a->cx, cy0 = a->cy;                   /* where the input starts */
    a->hist_pos = a->hist_n;                        /* start just past the newest */
    while (n < max) {
        uint64_t f = irq_save();                    /* check-and-block atomically */
        int c = iq_get(a);
        if (c < 0) { task_block(); irq_restore(f); continue; }  /* sleep until woken */
        irq_restore(f);
        if (c == '\n' || c == '\r') {
            if (n > 0) {                            /* save this line to history */
                int len = n < 95 ? (int)n : 95, slot;
                if (a->hist_n < 6) slot = a->hist_n++;
                else { for (int k = 1; k < 6; k++) memcpy(a->hist[k-1], a->hist[k], 96); slot = 5; }
                for (int i = 0; i < len; i++) a->hist[slot][i] = buf[i];
                a->hist[slot][len] = 0;
            }
            grid_putc(a, '\n'); buf[n++] = '\n'; break;
        }
        if (c == '\b' || c == 127) {
            if (n > 0) { n--; grid_erase(a, 1, cx0, cy0); }
            continue;
        }
        if (c == 0x11) {                            /* up-arrow: older command */
            if (a->hist_pos > 0) n = hist_recall(a, buf, max, n, --a->hist_pos, cx0, cy0);
            continue;
        }
        if (c == 0x12) {                            /* down-arrow: newer command */
            if (a->hist_pos < a->hist_n) n = hist_recall(a, buf, max, n, ++a->hist_pos, cx0, cy0);
            continue;
        }
        if (c == '\t') {                            /* Tab: complete a filename from the cwd */
            int ws = (int)n; while (ws > 0 && buf[ws-1] != ' ') ws--;
            int plen = (int)n - ws, slash = 0;
            for (int i = ws; i < (int)n; i++) if (buf[i] == '/') slash = 1;
            if (plen > 0 && !slash) {
                vfs_dirent e[32]; int ne = vfs_list(e, 32), mi = -1, nm = 0;
                for (int i = 0; i < ne; i++) {
                    int ok = 1;
                    for (int k = 0; k < plen; k++) {
                        char a1 = buf[ws+k], b1 = e[i].name[k];
                        if (a1 >= 'A' && a1 <= 'Z') a1 += 32;
                        if (b1 >= 'A' && b1 <= 'Z') b1 += 32;
                        if (a1 != b1) { ok = 0; break; }
                    }
                    if (ok) { nm++; mi = i; }
                }
                if (nm == 1) {                      /* unique match: replace with canonical name */
                    grid_erase(a, plen, cx0, cy0); n -= (unsigned)plen;
                    for (int k = 0; e[mi].name[k] && n + 1 < max; k++) {
                        char ch = e[mi].name[k];
                        if (ch == '/') continue;    /* drop the directory marker */
                        grid_putc(a, ch); buf[n++] = (char)ch;
                    }
                }
            }
            continue;
        }
        if (c < 32) continue;                       /* other control keys: ignore */
        grid_putc(a, (char)c); buf[n++] = (char)c;  /* echo */
    }
    return (int)n;
}

int  app_sys_getpid(void) { return cur()->pid; }
void app_sys_clear(void)  { grid_clear(cur()); }
void app_sys_exit(void)   { cur()->exited = 1; task_exit(); }

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
    grid_clear(a);
    a->cr3 = vmm_create_address_space();

    /* Load the ELF + user stack into the app's address space. We switch CR3 to
     * it (interrupts off) so the loader's writes land in the right space. */
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    uint64_t old;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old));
    __asm__ volatile("mov %0, %%cr3" : : "r"(a->cr3) : "memory");

    a->entry = elf_load(elf, elfsz);
    if (!a->entry) {                         /* bad ELF: don't spawn a null task */
        __asm__ volatile("mov %0, %%cr3" : : "r"(old) : "memory");
        __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
        a->used = 0;
        return 0;
    }
    for (int i = 0; i < USTACK_PAGES; i++)
        vmm_map(USTACK_BASE + (uint64_t)i * PAGE_SIZE, pmm_alloc_frame(),
                PTE_WRITABLE | PTE_USER);
    a->ustack = USTACK_BASE + USTACK_PAGES * PAGE_SIZE;

    __asm__ volatile("mov %0, %%cr3" : : "r"(old) : "memory");
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");

    /* 256 KB kernel stack (vs the 16 KB default): a ring-3 app's syscalls run on
     * its kernel stack, and SYS_https (shell get/wget) runs the TLS handshake +
     * bignum/RSA/ECDSA cert verification there — which overflows a small stack. */
    a->task = task_create_stack(app_trampoline, a->cr3, a, 256 * 1024);

    /* queue it for the window manager to give it a window */
    int n = (pend_h + 1) % MAX_APPS;
    if (n != pend_t) { pending[pend_h] = a; pend_h = n; }
    return a;
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
        if (eq && !*a && !*b)
            return app_spawn(progs[i].elf, progs[i].title, ~0ull) ? 0 : -1;  /* trusted embedded */
    }
    return -1;
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
