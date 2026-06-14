/* term.h — an in-window terminal widget (a small kernel-side shell). */
#pragma once
#include <stdint.h>

void term_init(void);
void term_input(char c);                          /* feed one keystroke */
void term_render(int px, int py, uint32_t fg, uint32_t bg);   /* draw the grid */
int  term_cols(void);
int  term_rows(void);
