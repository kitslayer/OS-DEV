/*
 * gfxdemo.c — a userspace graphics demo: the first program to draw real pixels.
 *
 * It asks the window manager for a pixel canvas (sys_gfx_init), renders an
 * animated plasma into a malloc'd framebuffer, and pushes each frame across with
 * sys_gfx_blit. Pure integer math (no FPU); a triangle-wave lookup stands in for
 * sine. Animation is paced by the millisecond clock. Press q or Esc to quit.
 *
 * This exercises the whole new stack at once — the heap (M496), the ms clock
 * (M497) and the graphics window API (M498) — and is the proof-of-concept the
 * DOOM port builds on.
 */
#include "ulib.h"

#define W 320
#define H 200

/* triangle wave: maps 0..255 -> 0..255 and back, a cheap stand-in for |sin| */
static int tri(int v) {
    v &= 255;
    return v < 128 ? v * 2 : 511 - v * 2;
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("gfxdemo: graphics init failed\n"); return 1; }
    unsigned int *fb = malloc((unsigned long)W * H * 4);
    if (!fb) { print("gfxdemo: out of memory\n"); return 1; }

    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 27) { free(fb); return 0; }

        int ph = (int)(sys_uptime_ms() / 16);          /* animation phase */
        for (int y = 0; y < H; y++) {
            int dy = y - H / 2;
            for (int x = 0; x < W; x++) {
                int dx = x - W / 2;
                int rad = (dx * dx + dy * dy) >> 7;     /* bounded radial term */
                int v1 = tri(x * 2 + ph);
                int v2 = tri(y * 2 - ph);
                int v3 = tri(rad + ph);
                int r = tri(v1 + v3);
                int g = tri(v2 + v3);
                int b = tri(v1 + v2 + ph);
                fb[y * W + x] = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
            }
        }
        sys_gfx_blit(fb);
        sys_sleep(30);                                  /* ~30 fps */
    }
}
