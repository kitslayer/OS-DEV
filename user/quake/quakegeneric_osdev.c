/*
 * quakegeneric_osdev.c — the platform layer binding quakegeneric (Quake) to this
 * OS.  Mirrors the DOOM port's doomgeneric_osdev.c, but for the quakegeneric
 * interface (quakegeneric.h):
 *
 *   - VID_Init (vid_null.c) calls QG_Init();  we set up the pixel canvas + raw
 *     keyboard.  VID_Update calls QG_DrawFrame(vid.buffer) — an 8-bit indexed
 *     320x240 buffer we convert through the palette to 0x00RRGGBB and blit.
 *   - IN_Commands (in_null.c) drains input via QG_GetKey / QG_GetMouseMove.
 *   - This file ALSO provides the Sys_* layer (we don't vendor sys_null.c): real
 *     timing from sys_uptime_ms, file IO via the libc-shim fopen (which maps a
 *     path to its uppercase 8.3 basename), and Sys_Error/Sys_Printf.
 *   - main(): QG_Create(argc,argv) (-> Host_Init via quakegeneric.c) then a loop
 *     computing dt from sys_uptime_ms and calling QG_Tick(dt) at ~72 fps.
 *
 * The engine renders at vid_null.c's BASEWIDTH x BASEHEIGHT = 320x240, so the
 * canvas + blit use QUAKEGENERIC_RES_X/RES_Y (320x240 from quakegeneric.h) to
 * match exactly — the window manager upscales it.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quakegeneric.h"
#include "quakekeys.h"

/* ---- syscalls from ulib (declared here; resolved at link against ulib.o). ---- */
extern int           sys_gfx_init(int w, int h);
extern int           sys_gfx_blit(const void *pixels);
extern void          sys_setkbmode(int raw);
extern int           sys_getkbevent(void);
extern unsigned long sys_uptime_ms(void);
extern void          sys_sleep(int ms);
extern void          sys_mouse_rel(int *dx, int *dy);
extern void          print(const char *s);
extern void          sys_exit(int code);

/* ===================================================================== */
/*  Video: 8-bit indexed -> 0x00RRGGBB                                    */
/* ===================================================================== */

static unsigned char qg_palette[768];                 /* 256 RGB triples */
static unsigned int  framebuf[QUAKEGENERIC_RES_X * QUAKEGENERIC_RES_Y];

void QG_Init(void)
{
    sys_gfx_init(QUAKEGENERIC_RES_X, QUAKEGENERIC_RES_Y);
    sys_setkbmode(1);                                  /* raw make/break scancodes */
}

void QG_Quit(void)
{
    /* The window manager reaps the task; nothing to tear down here. */
}

void QG_SetPalette(unsigned char palette[768])
{
    memcpy(qg_palette, palette, 768);
}

void QG_DrawFrame(void *pixels)
{
    const unsigned char *src = (const unsigned char *)pixels;
    for (int i = 0; i < QUAKEGENERIC_RES_X * QUAKEGENERIC_RES_Y; i++) {
        const unsigned char *e = &qg_palette[src[i] * 3];
        framebuf[i] = ((unsigned int)e[0] << 16) |
                      ((unsigned int)e[1] << 8)  |
                      ((unsigned int)e[2]);
    }
    sys_gfx_blit(framebuf);
}

/* ===================================================================== */
/*  Keyboard: PS/2 scancode set 1 -> Quake key codes (quakekeys.h)        */
/* ===================================================================== */

static int scancode_to_quakekey(int sc, int ext)
{
    if (ext) {
        switch (sc) {
        case 0x48: return K_UPARROW;
        case 0x50: return K_DOWNARROW;
        case 0x4B: return K_LEFTARROW;
        case 0x4D: return K_RIGHTARROW;
        case 0x1D: return K_CTRL;       /* right ctrl  */
        case 0x38: return K_ALT;        /* right alt   */
        case 0x1C: return K_ENTER;      /* keypad enter */
        case 0x47: return K_HOME;
        case 0x4F: return K_END;
        case 0x49: return K_PGUP;
        case 0x51: return K_PGDN;
        case 0x52: return K_INS;
        case 0x53: return K_DEL;
        default:   return 0;
        }
    }

    switch (sc) {
    /* control keys */
    case 0x01: return K_ESCAPE;
    case 0x1C: return K_ENTER;
    case 0x0F: return K_TAB;
    case 0x0E: return K_BACKSPACE;
    case 0x1D: return K_CTRL;            /* left ctrl */
    case 0x38: return K_ALT;             /* left alt  */
    case 0x39: return K_SPACE;           /* space     */
    case 0x2A: return K_SHIFT;           /* left shift  */
    case 0x36: return K_SHIFT;           /* right shift */

    /* non-extended arrows (numeric keypad without numlock) */
    case 0x48: return K_UPARROW;
    case 0x50: return K_DOWNARROW;
    case 0x4B: return K_LEFTARROW;
    case 0x4D: return K_RIGHTARROW;

    /* function keys */
    case 0x3B: return K_F1;
    case 0x3C: return K_F2;
    case 0x3D: return K_F3;
    case 0x3E: return K_F4;
    case 0x3F: return K_F5;
    case 0x40: return K_F6;
    case 0x41: return K_F7;
    case 0x42: return K_F8;
    case 0x43: return K_F9;
    case 0x44: return K_F10;
    case 0x57: return K_F11;
    case 0x58: return K_F12;

    /* punctuation on the main block (lowercased ASCII, like the SDL backend) */
    case 0x0C: return '-';
    case 0x0D: return '=';
    case 0x1A: return '[';
    case 0x1B: return ']';
    case 0x27: return ';';
    case 0x28: return '\'';
    case 0x29: return '`';
    case 0x2B: return '\\';
    case 0x33: return ',';
    case 0x34: return '.';
    case 0x35: return '/';

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

    /* letters -> lowercase ASCII */
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

int QG_GetKey(int *down, int *key)
{
    for (;;) {
        int ev = sys_getkbevent();
        if (ev < 0) return 0;                 /* queue empty */

        int released = ev & 0x100;
        int ext      = ev & 0x200;
        int sc       = ev & 0x7F;

        int k = scancode_to_quakekey(sc, ext);
        if (k == 0) continue;                 /* uninteresting key: try the next event */

        *down = released ? 0 : 1;
        *key  = k;
        return 1;
    }
}

void QG_GetMouseMove(int *x, int *y)
{
    sys_mouse_rel(x, y);                       /* relative motion (mouselook) */
}

void QG_GetJoyAxes(float *axes)
{
    for (int i = 0; i < QUAKEGENERIC_JOY_MAX_AXES; i++)
        axes[i] = 0.0f;                        /* no joystick */
}

/* ===================================================================== */
/*  Sys_* layer (we don't vendor sys_null.c — these are the real ones)    */
/* ===================================================================== */

/* Forward declarations (signatures match sys.h, which the engine TUs include). */
void Sys_Error(char *error, ...);
void Sys_Printf(char *fmt, ...);

/* qboolean from common.h is `int`-sized; declare the global the engine refers
 * to.  Single-player, never dedicated. */
int isDedicated = 0;

/* File IO: back Quake's integer handles with libc-shim FILE*s.  fopen reduces
 * the path to its uppercase 8.3 basename, so "id1/pak0.pak" -> "PAK0.PAK". */
#define MAX_HANDLES 16
static FILE *sys_handles[MAX_HANDLES];

static int findhandle(void)
{
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!sys_handles[i]) return i;
    return -1;                                 /* out of handles */
}

static int file_length(FILE *f)
{
    int pos = (int)ftell(f);
    fseek(f, 0, SEEK_END);
    int end = (int)ftell(f);
    fseek(f, pos, SEEK_SET);
    return end;
}

int Sys_FileOpenRead(char *path, int *hndl)
{
    int i = findhandle();
    if (i < 0) { *hndl = -1; return -1; }
    FILE *f = fopen(path, "rb");
    if (!f) { *hndl = -1; return -1; }
    sys_handles[i] = f;
    *hndl = i;
    return file_length(f);
}

int Sys_FileOpenWrite(char *path)
{
    int i = findhandle();
    if (i < 0) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) { Sys_Error("Error opening %s", path); return 0; }
    sys_handles[i] = f;
    return i;
}

void Sys_FileClose(int handle)
{
    if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle]) return;
    fclose(sys_handles[handle]);
    sys_handles[handle] = NULL;
}

void Sys_FileSeek(int handle, int position)
{
    if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle]) return;
    fseek(sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead(int handle, void *dest, int count)
{
    if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle]) return 0;
    return (int)fread(dest, 1, count, sys_handles[handle]);
}

int Sys_FileWrite(int handle, void *data, int count)
{
    if (handle <= 0 || handle >= MAX_HANDLES || !sys_handles[handle]) return 0;
    return (int)fwrite(data, 1, count, sys_handles[handle]);
}

int Sys_FileTime(char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return -1;
}

void Sys_mkdir(char *path) { (void)path; }

void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
    (void)startaddr; (void)length;             /* our pages are already RWX-ish to us */
}

void Sys_DebugLog(char *file, char *fmt, ...) { (void)file; (void)fmt; }

void Sys_Error(char *error, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, error);
    vsnprintf(buf, sizeof buf, error, ap);
    va_end(ap);
    print("Quake Sys_Error: ");
    print(buf);
    print("\n");
    sys_exit(1);
    for (;;) {}
}

void Sys_Printf(char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    print(buf);
}

void Sys_Quit(void)
{
    sys_exit(0);
    for (;;) {}
}

double Sys_FloatTime(void)
{
    /* Real wall-clock seconds since boot.  Quake uses this for the server
     * flush timeout at shutdown and (optionally) host_speeds profiling; the
     * per-frame simulation time comes from the dt we pass to QG_Tick. */
    return (double)sys_uptime_ms() / 1000.0;
}

char *Sys_ConsoleInput(void) { return NULL; }

void Sys_Sleep(void) { sys_sleep(1); }         /* yield briefly */

void Sys_SendKeyEvents(void)
{
    /* Input is pumped via IN_Commands()/QG_GetKey (in_null.c) each frame, so
     * there is nothing to do here. */
}

void Sys_HighFPPrecision(void) {}
void Sys_LowFPPrecision(void) {}

/* ===================================================================== */
/*  main()                                                                */
/* ===================================================================== */

int main(int argc, char **argv)
{
    /* basedir is "." in QG_Create; our fopen ignores the directory and maps any
     * path to its uppercase 8.3 basename ("id1/pak0.pak" -> "PAK0.PAK"), so Quake
     * finds the flat-filesystem data.  Quake's memory comes from QG_Create's
     * parms.memsize (32 MB, set in quakegeneric.c) — this engine ignores
     * -heapsize — so argv is just the program name. */
    static char *qargv[] = { "quake" };
    (void)argc; (void)argv;

    QG_Create(1, qargv);

    /* ~72 fps pacing to match Host_FilterTime's internal cap (it refuses to run
     * a frame faster than 1/72 s).  We compute the real elapsed dt and hand it
     * to QG_Tick (-> Host_Frame). */
    double oldtime = (double)sys_uptime_ms() / 1000.0 - 0.1;
    for (;;) {
        double newtime = (double)sys_uptime_ms() / 1000.0;
        double dt = newtime - oldtime;
        if (dt < 1.0 / 72.0) {                 /* don't spin: nap until the next slot */
            sys_sleep(1);
            continue;
        }
        QG_Tick(dt);
        oldtime = newtime;
    }
    return 0;
}
