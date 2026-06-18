/*
 * nes_osdev.c — the platform layer that binds libxnes (a pure-C NES emulator
 * core) to this OS, the same way doomgeneric_osdev.c binds DOOM.
 *
 * libxnes gives us a tiny, host-free core:
 *   - xnes_ctx_alloc(buf,len)  load an iNES ROM straight from a memory buffer;
 *   - xnes_step_frame(ctx)     emulate one video frame;
 *   - xnes_get_pixel(ctx,x,y)  read the 256x240 framebuffer — it returns
 *                              palette[ppu.front[...]], and the core's default
 *                              palette is already 0x00RRGGBB, exactly what
 *                              sys_gfx_blit wants (it masks the top byte);
 *   - xnes_controller_joystick_p1(ctl,down,up)  press/release joypad buttons.
 *
 * So this shim is: read GAME.NES off the FAT disk, open a 256x240 graphics
 * window, then loop { drain raw key events -> joypad; step one frame; blit }.
 * Timing is sys_uptime_ms / sys_sleep, paced to the NES's ~60 Hz.  Audio is a
 * deliberate follow-up (a no-op sink keeps the APU's callback non-NULL for now;
 * the next step wires it to sys_pcm_stream), mirroring how DOOM landed video
 * first and sound second.
 */

#include <xnes.h>

/* ---- syscalls from ulib (declared here to avoid pulling in ulib.h, which is
 * outside our -I path; the symbols resolve at link time against user_ulib.o). */
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

#define NES_W 256
#define NES_H 240

/*
 * Translate a PS/2 scancode-set-1 code (plus the E0 "extended" flag) into an
 * NES joypad button bit (XNES_JOYSTICK_*, from controller.h), or 0 if we don't
 * map it.  Controls: arrows = D-pad, X = A, Z = B, Enter = Start, Shift =
 * Select.  Esc (handled in the loop) quits.
 */
static uint8_t scancode_to_button(int sc, int ext)
{
    if (ext) {
        switch (sc) {
        case 0x48: return XNES_JOYSTICK_UP;
        case 0x50: return XNES_JOYSTICK_DOWN;
        case 0x4B: return XNES_JOYSTICK_LEFT;
        case 0x4D: return XNES_JOYSTICK_RIGHT;
        default:   return 0;
        }
    }
    switch (sc) {
    /* arrow cluster also arrives non-extended (keypad without numlock) */
    case 0x48: return XNES_JOYSTICK_UP;
    case 0x50: return XNES_JOYSTICK_DOWN;
    case 0x4B: return XNES_JOYSTICK_LEFT;
    case 0x4D: return XNES_JOYSTICK_RIGHT;
    case 0x2D: return XNES_JOYSTICK_A;        /* X -> A     */
    case 0x2C: return XNES_JOYSTICK_B;        /* Z -> B     */
    case 0x1C: return XNES_JOYSTICK_START;    /* Enter      */
    case 0x36: return XNES_JOYSTICK_SELECT;   /* right shift*/
    case 0x2A: return XNES_JOYSTICK_SELECT;   /* left shift */
    default:   return 0;
    }
}

/* A no-op audio sink: keeps xnes's per-sample callback non-NULL so the APU has
 * somewhere to push to, without producing sound yet (wired up next step). */
static void audio_sink(void *data, float v) { (void)data; (void)v; }

/* The framebuffer we hand to sys_gfx_blit each frame (0x00RRGGBB pixels). */
static uint32_t g_fb[NES_W * NES_H];

/* iNES ROMs we ship are well under 1 MiB; a static buffer sidesteps any
 * question of whether the core copies the ROM (it does) — this one outlives it
 * regardless. */
static unsigned char g_rom[1024 * 1024];

/* ---- ROM picker: list the .nes files on the FAT disk and choose one --------
 * The emulator is ROM-agnostic; any .nes dropped on the disk shows up here. */
static char rom_names[16][16];
static int  n_roms;

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void scan_roms(void) {
    static char buf[8192];
    long n = sys_list(buf, sizeof(buf));
    if (n < 0) n = 0;
    buf[n] = 0;
    n_roms = 0;
    int i = 0;
    while (buf[i] && n_roms < 16) {
        char nm[16]; int j = 0;
        while (buf[i] && buf[i] != ' ' && buf[i] != '\n' && j < 15) nm[j++] = buf[i++];
        nm[j] = 0;
        while (buf[i] && buf[i] != '\n') i++;       /* skip the "  size" column */
        if (buf[i] == '\n') i++;
        if (j >= 4 && nm[j-4] == '.' && up(nm[j-3]) == 'N' && up(nm[j-2]) == 'E' && up(nm[j-1]) == 'S') {
            for (int k = 0; k <= j; k++) rom_names[n_roms][k] = nm[k];
            n_roms++;
        }
    }
}

/* returns the chosen index, -1 if none on disk, -2 if the user quit */
static int pick_rom(void) {
    if (n_roms <= 1) return n_roms - 1;
    int sel = 0;
    for (;;) {
        sys_clear();
        sys_setcolor(4); print("\n  NES"); sys_setcolor(0); print("  - pick a ROM:\n\n");
        for (int i = 0; i < n_roms; i++) {
            if (i == sel) { sys_setcolor(11); print("   > "); }
            else          { sys_setcolor(8);  print("     "); }
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

int main(void)
{
    scan_roms();
    int idx = pick_rom();
    if (idx == -2) return 0;                          /* user quit the picker */
    if (idx < 0) { print("nes: no .nes ROM found on disk\n"); return 1; }

    long n = sys_readfile(rom_names[idx], g_rom, sizeof(g_rom));
    if (n <= 0) { print("nes: cannot read the ROM from disk\n"); return 1; }

    struct xnes_ctx_t *ctx = xnes_ctx_alloc(g_rom, (size_t)n);
    if (!ctx) { print("nes: not a valid iNES ROM (or unsupported mapper)\n"); return 1; }
    xnes_set_audio(ctx, 0, audio_sink, 48000);
    xnes_reset(ctx);

    if (sys_gfx_init(NES_W, NES_H) < 0) { print("nes: graphics init failed\n"); return 1; }
    sys_setkbmode(1);   /* raw make/break scancodes */

    /*
     * Input model.  An NES game samples the controller's *current* button state
     * once per frame (in its vblank handler), so a press must be visible for at
     * least one emulated frame to register.  Two states:
     *   held  — buttons physically down, carried across frames until their break;
     *   sent  — what we've actually pushed to the core (so we only diff-update).
     * The wrinkle: a synthetic/quick tap can deliver make AND break before we
     * next drain (one QEMU sendkey is a ~100 ms press+release), which would net
     * to "released" within a single frame and be missed by an edge-triggered
     * "Press Start".  So a button that was both pressed and released this frame
     * is still reported pressed for THIS frame and released next frame.
     */
    uint8_t held = 0, sent = 0;
    for (;;) {
        unsigned long t0 = sys_uptime_ms();

        uint8_t made = 0, broke = 0;
        int ev;
        while ((ev = sys_getkbevent()) >= 0) {
            int released = ev & 0x100;
            int ext      = ev & 0x200;
            int sc       = ev & 0x7F;
            if (sc == 0x01) { sys_setkbmode(0); return 0; }   /* Esc: quit app */
            uint8_t b = scancode_to_button(sc, ext);
            if (!b) continue;
            if (released) broke |= b; else made |= b;
        }
        held = (uint8_t)((held | made) & ~broke);
        uint8_t want = (uint8_t)(held | (made & broke));  /* same-frame tap pressed this frame */
        uint8_t down = (uint8_t)(want & ~sent), up = (uint8_t)(sent & ~want);
        if (down) xnes_controller_joystick_p1(&ctx->ctl, down, 0);
        if (up)   xnes_controller_joystick_p1(&ctx->ctl, 0, up);
        sent = want;

        xnes_step_frame(ctx);

        for (int y = 0; y < NES_H; y++)
            for (int x = 0; x < NES_W; x++)
                g_fb[y * NES_W + x] = xnes_get_pixel(ctx, x, y);
        sys_gfx_blit(g_fb);

        /* Pace to ~60 Hz (the NES runs at ~60.1; one 10 ms tick of slop is
         * imperceptible and keeps the scheduler fair). */
        unsigned long dt = sys_uptime_ms() - t0;
        if (dt < 16) sys_sleep((int)(16 - dt));
    }
}
