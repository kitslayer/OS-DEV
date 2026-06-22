/* desktop.h — a tiny windowing desktop environment. */
#pragma once

void desktop_run(void);   /* set up windows and run the event loop (no return) */

/* Load image file `name` as the desktop wallpaper at runtime (SYS_setwall):
 * decode + scale to the screen, swap in on success, keep the old on failure.
 * Returns 0 on success, -1 on any failure. */
int desktop_set_wallpaper(const char *name);
