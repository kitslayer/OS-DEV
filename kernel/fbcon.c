/*
 * fbcon.c — a scrolling text console drawn on the framebuffer.
 *
 * Once graphics mode is up, the kernel no longer has the VGA text grid; this
 * module recreates a terminal on top of raw pixels. It keeps a character grid
 * (columns × rows derived from the screen and font size) and a cursor, draws
 * each glyph with fb_glyph, and scrolls by shifting the framebuffer up a line.
 *
 * console.c routes all kernel/shell output here once graphics is enabled, so
 * everything you'd normally see in text mode now renders graphically.
 */
#include "fbcon.h"
#include "fb.h"
#include "font.h"

static int cols, rows, cx, cy;
static uint32_t fg = 0xD0D0D0, bg = 0x0A0A18;

void fbcon_set_colors(unsigned f, unsigned b) { fg = f; bg = b; }

int fbcon_init(void) {
    if (fb_init(1024, 768) != 0)
        return -1;
    cols = fb_width() / font_width;
    rows = fb_height() / font_height;
    cx = cy = 0;
    fb_clear(bg);
    return 0;
}

static void newline(void) {
    cx = 0;
    if (++cy >= rows) {
        fb_scroll(font_height, bg);
        cy = rows - 1;
    }
}

void fbcon_putc(char c) {
    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        cx = 0;
        break;
    case '\b':
        if (cx > 0) {
            cx--;
            fb_glyph(cx * font_width, cy * font_height, ' ', fg, bg);
        }
        break;
    case '\t':
        cx = (cx + 4) & ~3;
        if (cx >= cols)
            newline();
        break;
    default:
        fb_glyph(cx * font_width, cy * font_height, c, fg, bg);
        if (++cx >= cols)
            newline();
        break;
    }
}
