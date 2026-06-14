/*
 * vga.c — VGA text-mode terminal driver.
 *
 * In text mode the screen is a grid of 80x25 cells living in memory at
 * physical 0xB8000. Each cell is two bytes: the ASCII character, then an
 * "attribute" byte holding the foreground color (low nibble) and background
 * color (high nibble). Writing a cell instantly changes what's on screen —
 * there is no draw call, the video hardware scans this memory continuously.
 *
 * On top of that raw grid we add the things a "terminal" needs: a current
 * cursor position, line wrapping, newline/tab/backspace handling, and
 * scrolling when we run off the bottom. We also move the blinking hardware
 * cursor to match, which is done through the VGA CRT controller's I/O ports.
 */
#include "vga.h"
#include "io.h"
#include <stddef.h>

#define VGA_COLS   80
#define VGA_ROWS   25
#define VGA_MEM    ((volatile uint16_t *)0xB8000)

/* VGA CRT controller: write a register index to 0x3D4, its value to 0x3D5. */
#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5
#define CRTC_CURSOR_HI 14
#define CRTC_CURSOR_LO 15

static size_t  cursor_row;
static size_t  cursor_col;
static uint8_t color;          /* current attribute byte */

/* Pack a character and the current color into one VGA cell. */
static inline uint16_t make_cell(char c) {
    return (uint16_t)color << 8 | (uint8_t)c;
}

/* Tell the hardware where to draw the blinking cursor. */
static void move_hw_cursor(void) {
    uint16_t pos = (uint16_t)(cursor_row * VGA_COLS + cursor_col);
    outb(CRTC_INDEX, CRTC_CURSOR_LO);
    outb(CRTC_DATA,  (uint8_t)(pos & 0xFF));
    outb(CRTC_INDEX, CRTC_CURSOR_HI);
    outb(CRTC_DATA,  (uint8_t)(pos >> 8));
}

void vga_set_color(enum vga_color fg, enum vga_color bg) {
    color = (uint8_t)bg << 4 | (uint8_t)fg;
}

void vga_clear(void) {
    for (size_t i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEM[i] = make_cell(' ');
    cursor_row = cursor_col = 0;
    move_hw_cursor();
}

void vga_init(void) {
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

/* Copy every row up one, then blank the bottom row. */
static void scroll(void) {
    for (size_t r = 1; r < VGA_ROWS; r++)
        for (size_t c = 0; c < VGA_COLS; c++)
            VGA_MEM[(r - 1) * VGA_COLS + c] = VGA_MEM[r * VGA_COLS + c];

    for (size_t c = 0; c < VGA_COLS; c++)
        VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + c] = make_cell(' ');

    cursor_row = VGA_ROWS - 1;
}

/* Advance to the start of the next line, scrolling if we were on the last. */
static void newline(void) {
    cursor_col = 0;
    if (++cursor_row >= VGA_ROWS)
        scroll();
}

void vga_putc(char c) {
    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        cursor_col = 0;
        break;
    case '\t':
        cursor_col = (cursor_col + 4) & ~(size_t)3;  /* next 4-col stop */
        if (cursor_col >= VGA_COLS)
            newline();
        break;
    case '\b':
        if (cursor_col > 0)
            cursor_col--;
        VGA_MEM[cursor_row * VGA_COLS + cursor_col] = make_cell(' ');
        break;
    default:
        VGA_MEM[cursor_row * VGA_COLS + cursor_col] = make_cell(c);
        if (++cursor_col >= VGA_COLS)
            newline();
        break;
    }
    move_hw_cursor();
}
