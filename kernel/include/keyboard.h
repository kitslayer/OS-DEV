/* keyboard.h — PS/2 keyboard + the kernel's line-input buffer. */
#pragma once

void keyboard_init(void);

/* The shared input queue. Any device IRQ (keyboard, serial) pushes characters;
 * readers (the SYS_read syscall) pull them. */
void input_push(char c);
int  input_getchar(void);    /* blocking: returns the next char (sti/hlt waits) */
int  input_trygetchar(void); /* non-blocking: next char, or -1 if none ready */
