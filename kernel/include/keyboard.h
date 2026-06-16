/* keyboard.h — PS/2 keyboard + the kernel's line-input buffer. */
#pragma once

void keyboard_init(void);

/* The shared input queue. Any device IRQ (keyboard, serial) pushes characters;
 * readers (the SYS_read syscall) pull them. */
void input_push(char c);
int  input_getchar(void);    /* blocking: returns the next char (sti/hlt waits) */
int  input_trygetchar(void); /* non-blocking: next char, or -1 if none ready */

/* Raw make/break key events for games (DOOM), in parallel with the cooked queue
 * above. Each event: bits 0-6 = scancode, bit 8 (0x100) = released, bit 9 (0x200)
 * = extended (E0) key. The keyboard IRQ pushes every press AND release; the
 * window manager drains and routes them to a focused app that opted into raw
 * mode. */
void input_push_raw(unsigned short ev);
int  input_pop_raw(void);    /* non-blocking: next raw event, or -1 if none */
