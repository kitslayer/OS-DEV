/*
 * mouse.c — PS/2 mouse driver and a software cursor.
 *
 * The mouse hangs off the second PS/2 port, behind the same 8042 controller as
 * the keyboard, and raises **IRQ12**. Because IRQ12 is on the *slave* PIC, we
 * also have to unmask IRQ2 (the cascade line) for it to reach the CPU.
 *
 * The mouse streams 3-byte packets: a flags byte (button states + movement
 * sign bits) and signed X/Y deltas. We accumulate them into an absolute screen
 * position, clamped to the framebuffer, and draw an arrow cursor that saves and
 * restores the pixels underneath it so it floats over whatever's on screen.
 */
#include "mouse.h"
#include "interrupts.h"
#include "pic.h"
#include "io.h"
#include "fb.h"
#include <stdint.h>

#define PS2_DATA 0x60
#define PS2_CMD  0x64

/* Bounded waits — never hang if the controller doesn't behave as expected. */
static void ps2_wait_in(void) {
    for (int i = 0; i < 100000 && (inb(PS2_CMD) & 2); i++) {}
}
static int ps2_wait_out(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_CMD) & 1)
            return 1;
    return 0;
}

static void ps2_flush(void) {
    for (int i = 0; i < 16 && (inb(PS2_CMD) & 1); i++)
        inb(PS2_DATA);
}

static void mouse_command(uint8_t cmd) {
    ps2_wait_in(); outb(PS2_CMD, 0xD4);     /* "next byte goes to the mouse" */
    ps2_wait_in(); outb(PS2_DATA, cmd);
    if (ps2_wait_out())                      /* read ACK (0xFA) if it comes */
        inb(PS2_DATA);
}

static int      mx, my, buttons;
static volatile int rel_dx, rel_dy;        /* raw motion since the last mouse_read_rel */
static uint8_t  packet[3];
static int      phase;

int mouse_x(void)       { return mx; }
int mouse_y(void)       { return my; }
int mouse_buttons(void) { return buttons; }

/* Relative motion accumulated since the previous call (read + clear). Used for
 * mouselook in games, where absolute clamped-to-screen position can't turn past
 * the edge. */
void mouse_read_rel(int *dx, int *dy) {
    uint64_t fl; __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    if (dx) *dx = rel_dx;
    if (dy) *dy = rel_dy;
    rel_dx = rel_dy = 0;
    __asm__ volatile("push %0; popfq" : : "r"(fl) : "memory", "cc");
}

void mouse_set_abs(int x, int y, int b) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > fb_width() - 1)  x = fb_width() - 1;
    if (y > fb_height() - 1) y = fb_height() - 1;
    mx = x; my = y; buttons = b;
}

static void handle_packet(void) {
    buttons = packet[0] & 0x07;

    /* Bits 6/7 are the X/Y overflow flags. On a fast flick the delta exceeds
     * what fits in one byte; the reported values are then garbage. Using them
     * makes the cursor teleport — so drop movement for that packet. */
    if (packet[0] & 0xC0)
        return;

    int dx = packet[1];
    int dy = packet[2];
    if (packet[0] & 0x10) dx |= ~0xFF;       /* sign-extend X */
    if (packet[0] & 0x20) dy |= ~0xFF;       /* sign-extend Y */

    rel_dx += dx;                            /* accumulate raw motion for mouselook */
    rel_dy += dy;
    mx += dx;
    my -= dy;                                /* screen Y grows downward */
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > fb_width() - 1)  mx = fb_width() - 1;
    if (my > fb_height() - 1) my = fb_height() - 1;
}

static void mouse_handler(struct registers *r) {
    (void)r;
    if (!(inb(PS2_CMD) & 0x20))               /* 0x20 = byte came from the mouse */
        return;
    uint8_t data = inb(PS2_DATA);

    switch (phase) {
    case 0:
        if (!(data & 0x08))                   /* bit3 always set in byte 0: resync */
            return;
        packet[0] = data; phase = 1; break;
    case 1:
        packet[1] = data; phase = 2; break;
    case 2:
        packet[2] = data; phase = 0;
        handle_packet();
        break;
    }
}

void mouse_init(void) {
    ps2_flush();                              /* clear any stale byte */
    ps2_wait_in(); outb(PS2_CMD, 0xA8);       /* enable the auxiliary (mouse) port */

    ps2_wait_in(); outb(PS2_CMD, 0x20);       /* read controller config byte */
    uint8_t cfg = ps2_wait_out() ? inb(PS2_DATA) : 0;
    cfg |= 0x02;                              /* enable IRQ12 */
    cfg &= ~0x20;                             /* enable the mouse clock */
    ps2_wait_in(); outb(PS2_CMD, 0x60);       /* write config byte back */
    ps2_wait_in(); outb(PS2_DATA, cfg);

    mouse_command(0xF6);                      /* set defaults */
    mouse_command(0xE6);                      /* scaling 1:1 (linear, not 2:1 accel) */
    mouse_command(0xE8); mouse_command(0x02); /* resolution = 4 counts/mm */
    mouse_command(0xF3); mouse_command(0x3C); /* sample rate = 60 reports/s */
    mouse_command(0xF4);                      /* enable data reporting */

    mx = fb_width() / 2;
    my = fb_height() / 2;

    pic_unmask(2);                            /* cascade line for the slave PIC */
    irq_install_handler(12, mouse_handler);   /* unmasks IRQ12 */
}

/* --- arrow cursor (X = black outline, O = white fill, space = transparent) --- */
#define CW 12
#define CH 19
static const char *cursor_img[CH] = {
    "X           ", "XX          ", "XOX         ", "XOOX        ",
    "XOOOX       ", "XOOOOX      ", "XOOOOOX     ", "XOOOOOOX    ",
    "XOOOOOOOX   ", "XOOOOOOOOX  ", "XOOOOOOOOOX ", "XOOOOOXXXXXX",
    "XOOXOOX     ", "XOX XOOX    ", "XX  XOOX    ", "X    XOOX   ",
    "     XOOX   ", "      XOOX  ", "      XXXX  ",
};

static uint32_t under[CW * CH];
static int saved_x = -1, saved_y, have_saved;

static void cursor_restore(void) {
    if (!have_saved) return;
    for (int j = 0; j < CH; j++)
        for (int i = 0; i < CW; i++)
            fb_pixel(saved_x + i, saved_y + j, under[j * CW + i]);
    have_saved = 0;
}

void mouse_render(void) {
    cursor_restore();
    int x = mx, y = my;
    for (int j = 0; j < CH; j++)
        for (int i = 0; i < CW; i++)
            under[j * CW + i] = fb_get_pixel(x + i, y + j);
    saved_x = x; saved_y = y; have_saved = 1;

    for (int j = 0; j < CH; j++)
        for (int i = 0; i < CW; i++) {
            char p = cursor_img[j][i];
            if (p == 'X') fb_pixel(x + i, y + j, 0x000000);
            else if (p == 'O') fb_pixel(x + i, y + j, 0xFFFFFF);
        }
}

/* Paint the cursor at an explicit position. The compositor passes a single
 * snapshot of (mx,my) so the painted pixels, the rectangle it flushes, and the
 * position it caches all agree — even if a PS/2 mouse IRQ updates mx/my mid-frame. */
void mouse_paint_at(int x, int y) {
    for (int j = 0; j < CH; j++)
        for (int i = 0; i < CW; i++) {
            char p = cursor_img[j][i];
            if (p == 'X') fb_pixel(x + i, y + j, 0x000000);
            else if (p == 'O') fb_pixel(x + i, y + j, 0xFFFFFF);
        }
}

void mouse_paint(void) { mouse_paint_at(mx, my); }

int mouse_cursor_w(void) { return CW; }
int mouse_cursor_h(void) { return CH; }
