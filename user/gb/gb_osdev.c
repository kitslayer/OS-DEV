/*
 * gb_osdev.c — the platform layer binding Peanut-GB (a single-header MIT
 * Game Boy emulator) to this OS, the same way nes_osdev.c binds the NES core.
 *
 * Peanut-GB calls back for ROM / cart-RAM bytes (cb_rom / cb_ram_*) and emits
 * the 160x144 screen one scanline at a time (draw_line); we serve the ROM from
 * a disk-loaded buffer, map each 2-bit shade to a classic DMG-green colour into
 * a framebuffer, and push it out sys_gfx_blit each frame.  Input is the raw
 * PS/2 make/break queue mapped onto the joypad byte (active-low), with the same
 * same-frame-tap latch as the NES so quick taps and held keys both register.
 */
#include <peanut_gb.h>

extern int           sys_gfx_init(int w, int h);
extern int           sys_gfx_blit(const void *pixels);
extern void          sys_setkbmode(int raw);
extern int           sys_getkbevent(void);
extern unsigned long sys_uptime_ms(void);
extern void          sys_sleep(int ms);
extern long          sys_readfile(const char *name, void *buf, unsigned long len);
extern long          sys_list(void *buf, unsigned long len);
extern int           sys_pollkey(void);
extern void          sys_setcolor(int color);
extern void          sys_clear(void);
extern void          print(const char *s);

#define GB_W 160
#define GB_H 144

struct priv { uint8_t *rom; uint8_t *ram; };

static uint8_t  g_rom[1024 * 1024];   /* GB ROM (libbet is 32 KB; most homebrew is well under 1 MB) */
static uint8_t  g_ram[32 * 1024];     /* cartridge RAM (max for an MBC) */
static uint32_t g_fb[GB_W * GB_H];
/* the classic DMG green, shades 0 (lightest) .. 3 (darkest) */
static const uint32_t g_pal[4] = { 0x9bbc0f, 0x8bac0f, 0x306230, 0x0f380f };

static uint8_t cb_rom(struct gb_s *gb, const uint_fast32_t a) { return ((struct priv *)gb->direct.priv)->rom[a]; }
static uint8_t cb_ram_r(struct gb_s *gb, const uint_fast32_t a) { return ((struct priv *)gb->direct.priv)->ram[a]; }
static void    cb_ram_w(struct gb_s *gb, const uint_fast32_t a, const uint8_t v) { ((struct priv *)gb->direct.priv)->ram[a] = v; }
static void    cb_err(struct gb_s *gb, const enum gb_error_e e, const uint16_t a) { (void)gb; (void)e; (void)a; }

static void draw_line(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
    (void)gb;
    uint32_t *row = &g_fb[(unsigned)line * GB_W];
    for (int x = 0; x < GB_W; x++) row[x] = g_pal[pixels[x] & 3];   /* low 2 bits = shade */
}

/* a PS/2 scancode (+ E0 ext) -> a JOYPAD_* bit (from peanut_gb.h), or 0 */
static uint8_t scancode_to_pad(int sc, int ext) {
    if (ext) { switch (sc) {
        case 0x48: return JOYPAD_UP;   case 0x50: return JOYPAD_DOWN;
        case 0x4B: return JOYPAD_LEFT; case 0x4D: return JOYPAD_RIGHT;
        default:   return 0; } }
    switch (sc) {
    case 0x48: return JOYPAD_UP;   case 0x50: return JOYPAD_DOWN;
    case 0x4B: return JOYPAD_LEFT; case 0x4D: return JOYPAD_RIGHT;
    case 0x2D: return JOYPAD_A;        /* X -> A     */
    case 0x2C: return JOYPAD_B;        /* Z -> B     */
    case 0x1C: return JOYPAD_START;    /* Enter      */
    case 0x36: case 0x2A: return JOYPAD_SELECT;   /* shift -> Select */
    default:   return 0;
    }
}

/* ---- ROM picker (.gb on the FAT disk), mirroring the NES one ---- */
static char rom_names[16][16];
static int  n_roms;
static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static void scan_roms(void) {
    static char buf[8192];
    long n = sys_list(buf, sizeof(buf)); if (n < 0) n = 0; buf[n] = 0;
    n_roms = 0; int i = 0;
    while (buf[i] && n_roms < 16) {
        char nm[16]; int j = 0;
        while (buf[i] && buf[i] != ' ' && buf[i] != '\n' && j < 15) nm[j++] = buf[i++];
        nm[j] = 0;
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
        if (j >= 3 && nm[j-3] == '.' && upc(nm[j-2]) == 'G' && upc(nm[j-1]) == 'B') {
            for (int k = 0; k <= j; k++) rom_names[n_roms][k] = nm[k];
            n_roms++;
        }
    }
}
static int pick_rom(void) {
    if (n_roms <= 1) return n_roms - 1;
    int sel = 0;
    for (;;) {
        sys_clear();
        sys_setcolor(4); print("\n  Game Boy"); sys_setcolor(0); print("  - pick a ROM:\n\n");
        for (int i = 0; i < n_roms; i++) {
            if (i == sel) { sys_setcolor(11); print("   > "); } else { sys_setcolor(8); print("     "); }
            print(rom_names[i]); print("\n");
        }
        sys_setcolor(0); print("\n  up/down move   Enter play   q quit\n");
        int k; while ((k = sys_pollkey()) < 0) sys_sleep(20);
        if      (k == 'q' || k == 'Q') return -2;
        else if (k == 0x11) { if (sel > 0) sel--; }
        else if (k == 0x12) { if (sel < n_roms - 1) sel++; }
        else if (k == '\n' || k == '\r' || k == ' ') return sel;
    }
}

int main(void) {
    scan_roms();
    int idx = pick_rom();
    if (idx == -2) return 0;
    if (idx < 0) { print("gb: no .gb ROM found on disk\n"); return 1; }
    long n = sys_readfile(rom_names[idx], g_rom, sizeof(g_rom));
    if (n <= 0) { print("gb: cannot read the ROM from disk\n"); return 1; }

    static struct gb_s gb;
    struct priv pv = { g_rom, g_ram };
    if (gb_init(&gb, cb_rom, cb_ram_r, cb_ram_w, cb_err, &pv) != GB_INIT_NO_ERROR) {
        print("gb: init failed (unsupported cartridge?)\n"); return 1;
    }
    gb_init_lcd(&gb, draw_line);

    if (sys_gfx_init(GB_W, GB_H) < 0) { print("gb: graphics init failed\n"); return 1; }
    sys_setkbmode(1);
    gb.direct.joypad = 0xFF;          /* all buttons released (active-low) */

    uint8_t held = 0;
    for (;;) {
        unsigned long t0 = sys_uptime_ms();

        uint8_t made = 0, broke = 0; int ev;
        while ((ev = sys_getkbevent()) >= 0) {
            int rel = ev & 0x100, ext = ev & 0x200, sc = ev & 0x7F;
            if (sc == 0x01) { sys_setkbmode(0); return 0; }   /* Esc quits */
            uint8_t b = scancode_to_pad(sc, ext);
            if (!b) continue;
            if (rel) broke |= b; else made |= b;
        }
        held = (uint8_t)((held | made) & ~broke);
        uint8_t active = (uint8_t)(held | (made & broke));    /* same-frame tap still pressed this frame */
        gb.direct.joypad = (uint8_t)~active;                  /* active-low */

        gb_run_frame(&gb);
        sys_gfx_blit(g_fb);

        unsigned long dt = sys_uptime_ms() - t0;              /* ~60 Hz pacing */
        if (dt < 16) sys_sleep((int)(16 - dt));
    }
}
