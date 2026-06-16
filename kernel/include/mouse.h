/* mouse.h — PS/2 mouse driver + arrow cursor. */
#pragma once

void mouse_init(void);
int  mouse_x(void);
int  mouse_y(void);
int  mouse_buttons(void);     /* bit0 left, bit1 right, bit2 middle */
void mouse_read_rel(int *dx, int *dy);  /* raw motion since last call (read+clear); for mouselook */

/* Set the pointer to an absolute position (used by the USB tablet, which
 * reports absolute coordinates instead of relative motion). */
void mouse_set_abs(int x, int y, int buttons);

/* Redraw the cursor at the current position (restores what was underneath the
 * previous spot first). Call after the background has settled. */
void mouse_render(void);

/* Paint the cursor with no save/restore — for a compositor that redraws the
 * whole frame anyway. `mouse_paint_at` takes an explicit position so the caller
 * can use one consistent snapshot of the (async-updated) cursor coordinates. */
void mouse_paint(void);
void mouse_paint_at(int x, int y);

/* Cursor bitmap dimensions, so a compositor can flush just the cursor rect. */
int  mouse_cursor_w(void);
int  mouse_cursor_h(void);
