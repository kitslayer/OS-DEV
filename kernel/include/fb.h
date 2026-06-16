/* fb.h — linear framebuffer graphics. */
#pragma once
#include <stdint.h>

int      fb_init(uint16_t width, uint16_t height);   /* set the video mode; 0 ok */
int      fb_width(void);
int      fb_height(void);
int      fb_save_bmp(const char *name);   /* screenshot the live screen to a 24-bit BMP file; 0/-1 */

void     fb_clear(uint32_t color);
void     fb_pixel(int x, int y, uint32_t color);
uint32_t fb_get_pixel(int x, int y);
void     fb_fill_rect(int x, int y, int w, int h, uint32_t color);

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
