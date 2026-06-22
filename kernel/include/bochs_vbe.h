/*
 * bochs_vbe.h — Bochs/QEMU VBE (DISPI) display driver.
 *
 * The "DISPI" interface (exposed by QEMU's std-VGA / bochs-display, PCI
 * 1234:1111) lets the OS pick its own video mode at runtime — resolution +
 * 32 bpp — instead of being stuck at whatever the boot console first set.
 * After a mode-set the linear framebuffer lives at the VGA card's BAR0; this
 * driver programs the mode, locates the LFB, and re-points fb.c at it.
 */
#pragma once

/* Is the DISPI interface present (so a mode-set is possible)?  1 yes / 0 no. */
int bochs_vbe_available(void);

/* Set a 32-bpp linear video mode of w*h pixels: disable DISPI, program
 * XRES/YRES/BPP=32/VIRT_WIDTH=w, re-enable with the LFB on, locate the LFB
 * (the VGA's BAR0), map it, and re-point fb.c (base/width/height/pitch=w*4) so
 * every draw primitive and the desktop come up at the new mode. Validates that
 * w/h are sane and the LFB is large enough. Returns 0 on success, -1 if the
 * interface is unavailable or the request is unsupported (in which case the
 * current mode is left untouched — never a black screen). */
int bochs_vbe_set_mode(int w, int h);
