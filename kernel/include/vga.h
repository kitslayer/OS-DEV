/* vga.h — 80x25 VGA text-mode terminal. */
#pragma once
#include <stdint.h>

/* The 16 VGA text colors. Used for both foreground and background. */
enum vga_color {
    VGA_BLACK = 0,  VGA_BLUE,        VGA_GREEN,       VGA_CYAN,
    VGA_RED,        VGA_MAGENTA,     VGA_BROWN,       VGA_LIGHT_GREY,
    VGA_DARK_GREY,  VGA_LIGHT_BLUE,  VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED,  VGA_LIGHT_MAGENTA, VGA_YELLOW,    VGA_WHITE,
};

void vga_init(void);                          /* clear screen, reset cursor */
void vga_clear(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putc(char c);                        /* handles \n, \r, \t, \b, scroll */
