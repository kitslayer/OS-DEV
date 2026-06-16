/*
 * doomgeneric_osdev.c — the platform layer that binds doomgeneric to this OS.
 *
 * Implements the six DG_* hooks declared in doomgeneric.h plus main():
 *   - the framebuffer goes out through sys_gfx_blit (a 320x200 XRGB canvas);
 *   - timing comes from sys_uptime_ms / sys_sleep;
 *   - input is drained from the raw PS/2 make/break event queue
 *     (sys_setkbmode(1) + sys_getkbevent) and translated to DOOM key codes.
 *
 * Pixel format: DOOM (i_video.c, rgba8888 path) packs each pixel as
 *   (r << 16) | (g << 8) | (b << 0)  ==  0x00RRGGBB,
 * which is exactly what sys_gfx_blit wants (it masks off the top byte). So
 * DG_DrawFrame is a straight passthrough — no swizzle.
 */

#include <stdint.h>

#include "doomgeneric.h"
#include "doomkeys.h"

/* ---- syscalls from ulib (declared here to avoid pulling in ulib.h, which is
 * outside our -I path; the symbols resolve at link time against user_ulib.o) */
extern int           sys_gfx_init(int w, int h);
extern int           sys_gfx_blit(const void *pixels);
extern void          sys_setkbmode(int raw);
extern int           sys_getkbevent(void);
extern unsigned long sys_uptime_ms(void);
extern void          sys_sleep(int ms);

/* DOOM sound mixer pump (user/doom/i_sound_osdev.c): mixes the active sound
 * channels to 48 kHz stereo and feeds the kernel's streaming PCM ring.  Called
 * once per frame so the ~1-second ring stays topped up at ~70 fps. */
extern void          osdev_sound_pump(void);

/* ----------------------------------------------------------------------- */

void DG_Init(void)
{
    sys_gfx_init(DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    sys_setkbmode(1);   /* raw make/break scancodes */
}

void DG_DrawFrame(void)
{
    /* Keep the audio ring fed first — non-blocking; mixes only while there's
     * room, so this is cheap when the ring is already full. */
    osdev_sound_pump();

    /* DG_ScreenBuffer is already 0x00RRGGBB per pixel — pass it straight. */
    sys_gfx_blit(DG_ScreenBuffer);
}

void DG_SleepMs(uint32_t ms)
{
    sys_sleep((int)ms);
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)sys_uptime_ms();
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;   /* no per-window title API; ignore */
}

/*
 * Translate a PS/2 scancode-set-1 code (plus the "extended" flag from an
 * E0 prefix) into a DOOM key code (doomkeys.h).  Returns 0 for keys we don't
 * care about.
 */
static unsigned char scancode_to_doomkey(int sc, int ext)
{
    if (ext) {
        /* E0-prefixed keys: the arrow cluster (and a few extras). */
        switch (sc) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1D: return KEY_FIRE;     /* right ctrl  */
        case 0x38: return KEY_RALT;     /* right alt   */
        case 0x1C: return KEY_ENTER;    /* keypad enter*/
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INS;
        case 0x53: return KEY_DEL;
        default:   return 0;
        }
    }

    switch (sc) {
    /* control keys */
    case 0x01: return KEY_ESCAPE;
    case 0x1C: return KEY_ENTER;
    case 0x0F: return KEY_TAB;
    case 0x0E: return KEY_BACKSPACE;
    case 0x1D: return KEY_FIRE;          /* left ctrl  -> fire   */
    case 0x38: return KEY_RALT;          /* left alt   -> strafe */
    case 0x39: return KEY_USE;           /* space      -> use    */
    case 0x2A: return KEY_RSHIFT;        /* left shift -> run    */
    case 0x36: return KEY_RSHIFT;        /* right shift-> run    */

    /* non-extended arrows (numeric keypad without numlock) */
    case 0x48: return KEY_UPARROW;
    case 0x50: return KEY_DOWNARROW;
    case 0x4B: return KEY_LEFTARROW;
    case 0x4D: return KEY_RIGHTARROW;

    /* function keys */
    case 0x3B: return KEY_F1;
    case 0x3C: return KEY_F2;
    case 0x3D: return KEY_F3;
    case 0x3E: return KEY_F4;
    case 0x3F: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;

    case 0x0C: return KEY_MINUS;
    case 0x0D: return KEY_EQUALS;

    /* number row 1..0 */
    case 0x02: return '1';
    case 0x03: return '2';
    case 0x04: return '3';
    case 0x05: return '4';
    case 0x06: return '5';
    case 0x07: return '6';
    case 0x08: return '7';
    case 0x09: return '8';
    case 0x0A: return '9';
    case 0x0B: return '0';

    /* letters (scancode-set-1 layout) -> lowercase ASCII */
    case 0x10: return 'q';
    case 0x11: return 'w';
    case 0x12: return 'e';
    case 0x13: return 'r';
    case 0x14: return 't';
    case 0x15: return 'y';
    case 0x16: return 'u';
    case 0x17: return 'i';
    case 0x18: return 'o';
    case 0x19: return 'p';
    case 0x1E: return 'a';
    case 0x1F: return 's';
    case 0x20: return 'd';
    case 0x21: return 'f';
    case 0x22: return 'g';
    case 0x23: return 'h';
    case 0x24: return 'j';
    case 0x25: return 'k';
    case 0x26: return 'l';
    case 0x2C: return 'z';
    case 0x2D: return 'x';
    case 0x2E: return 'c';
    case 0x2F: return 'v';
    case 0x30: return 'b';
    case 0x31: return 'n';
    case 0x32: return 'm';

    default:   return 0;
    }
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    int ev = sys_getkbevent();
    if (ev < 0) return 0;

    int released = ev & 0x100;
    int ext      = ev & 0x200;
    int sc       = ev & 0x7F;

    unsigned char k = scancode_to_doomkey(sc, ext);
    if (k == 0) return 0;            /* uninteresting key: skip (caller loops) */

    *pressed = released ? 0 : 1;
    *doomKey = k;
    return 1;
}

int main(void)
{
    char *argv[] = { "doom", "-iwad", "DOOM1.WAD" };
    doomgeneric_Create(3, argv);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
