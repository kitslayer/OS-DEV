/* fb.h — linear framebuffer graphics. */
#pragma once
#include <stdint.h>

int      fb_init(uint16_t width, uint16_t height);   /* set the video mode; 0 ok */
int      fb_init_mb(uint64_t base, int w, int h, int pitch, int bpp);  /* use a Multiboot/GRUB LFB (bare metal); 0 ok / -1 (M1292) */
/* Re-point the framebuffer at a new linear-framebuffer base of w*h 32-bpp
 * pixels (maps the region + updates dims/LFB pointer). The DISPI driver
 * (bochs_vbe.c) calls this after a mode-set; pitch is implicit (w*4), so the
 * caller must program the hardware VIRT_WIDTH = w to match. */
void     fb_repoint(uint64_t base, int w, int h);
int      fb_width(void);
int      fb_height(void);
int      fb_save_bmp(const char *name);   /* screenshot the live screen to a 24-bit BMP file; 0/-1 */
int      fb_save_bmp_buf(const char *name, const uint32_t *src, int w, int h);  /* save a caller's w*h 0x00RRGGBB buffer as a 24-bit BMP; 0/-1 */
int      fb_save_png(const char *name);   /* screenshot the live screen to a PNG file; 0/-1 */

void     fb_clear(uint32_t color);
void     fb_pixel(int x, int y, uint32_t color);
uint32_t fb_get_pixel(int x, int y);
void     fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void     fb_darken_rect(int x, int y, int w, int h, int pct);   /* scale existing pixels' RGB by pct/100 in place (soft shadows) */
void     fb_blit_scaled(int x, int y, const uint32_t *src, int sw, int sh, int scale);  /* nearest-neighbour blit, masked to 24-bit */
void     fb_row(int x, int y, int w, const uint32_t *colors);  /* write w explicit (caller-computed) colours into one row */
void     fb_set_clip(int x0, int y0, int x1, int y1);   /* bound draws to [x0,x1)x[y0,y1) screen px (default: whole screen) */
void     fb_reset_clip(void);

/* Draw character `c` (8x16 font) at pixel (x,y): set bits in fg, rest in bg. */
void     fb_glyph(int x, int y, char c, uint32_t fg, uint32_t bg);
void     fb_glyph_fg(int x, int y, char c, uint32_t fg);  /* transparent: set pixels only */
/* Draw a string with the font scaled up (for titles); transparent background. */
void     fb_text(int x, int y, const char *s, uint32_t color, int scale);

/* Scroll the whole framebuffer up by `px` pixels, filling the gap with bg. */
void     fb_scroll(int px, uint32_t bg);

/* Double buffering: point drawing at an off-screen buffer, then blit it to the
 * real screen in one shot (no flicker/tearing). Pass NULL to draw direct. */
void     fb_set_target(uint32_t *backbuffer);
void     fb_present(void);   /* copy the back buffer to the visible screen */
void     fb_present_rect(int x, int y, int w, int h);  /* copy just one rect */
