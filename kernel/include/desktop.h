/* desktop.h — a tiny windowing desktop environment. */
#pragma once
#include <stdint.h>

void desktop_run(void);   /* set up windows and run the event loop (no return) */

/* Load image file `name` as the desktop wallpaper at runtime (SYS_setwall):
 * decode + scale to the screen, swap in on success, keep the old on failure.
 * Returns 0 on success, -1 on any failure. */
int desktop_set_wallpaper(const char *name);

/* Decode image file `name`, fit-scale it (preserving aspect, letterboxed) into
 * the cw*ch XRGB pixel buffer `buf`, and report the native pixel size in
 * outwh[0]=width, outwh[1]=height (SYS_loadimg, for the image viewer). Reuses
 * decode_image() so every format the OS reads (PNG/BMP/JPEG/GIF/SVG) works.
 * Returns 0 on success, -1 on any failure. */
int desktop_load_image(const char *name, unsigned *buf, int cw, int ch, int *outwh);

/* The wallpaper's own color at absolute screen coords (x,y) (THEME_VOID if
 * out of range or not yet generated) — for a translucent window background
 * that wants to blend toward the real backdrop, not toward its own last
 * frame (kernel/app.c's terminal-cell rendering, M1527). */
uint32_t desktop_wallpaper_sample(int x, int y);
