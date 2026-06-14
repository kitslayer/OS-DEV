/* fbcon.h — a text console rendered on the framebuffer. */
#pragma once

int  fbcon_init(void);          /* set graphics mode + clear; 0 on success */
void fbcon_putc(char c);        /* draw a character, handling \n \r \b \t + scroll */
void fbcon_set_colors(unsigned fg, unsigned bg);
