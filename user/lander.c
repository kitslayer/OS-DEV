/*
 * lander.c — Lunar Lander, drawn in the framebuffer.
 *
 * Ride gravity down and touch the green landing pad gently: keep the descent
 * slow and nearly vertical. The main engine (Up) burns fuel and pushes up; the
 * side thrusters (Left/Right) nudge you horizontally. Land too fast, too
 * sideways, or off the pad and you crash. Land softly to win.
 *
 * Up = thrust, Left/Right = side thrusters, r = restart, Esc/q = quit.
 * Built with SSE (its own Makefile rule) for the float physics; raw make/break
 * input with the same same-frame-tap latch as the other graphics apps.
 */

extern int           sys_gfx_init(int w, int h);
extern int           sys_gfx_blit(const void *pixels);
extern void          sys_setkbmode(int raw);
extern int           sys_getkbevent(void);
extern unsigned long sys_uptime_ms(void);
extern void          sys_sleep(int ms);
extern void          sys_beep(int hz, int ms);
extern void          print(const char *s);

#define W 320
#define H 200
#define GROUND (H - 24)
#define PAD0 132
#define PAD1 188
#define SAFE_VY 1.3f
#define SAFE_VX 0.9f

static unsigned fb[W * H];
static void plot(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = c; }
static void line(int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1 - x0, dy = y1 - y0; dx = dx < 0 ? -dx : dx; dy = dy < 0 ? -dy : dy;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) { plot(x0, y0, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * err; if (e2 > -dy) { err -= dy; x0 += sx; } if (e2 < dx) { err += dx; y0 += sy; } }
}
static void rect(int x, int y, int w, int h, unsigned c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) plot(x + i, y + j, c); }

int main(void) {
    float x, y, vx, vy, fuel; int over, won;
    int hu = 0, hl = 0, hr = 0;

    if (sys_gfx_init(W, H) < 0) { print("lander: gfx init failed\n"); return 1; }
    sys_setkbmode(1);

    #define RESET() do { x = 40; y = 24; vx = 0.6f; vy = 0; fuel = 1000; over = 0; won = 0; } while (0)
    RESET();

    for (;;) {
        unsigned long t0 = sys_uptime_ms();
        int ev, mu = 0, ml = 0, mr = 0;
        while ((ev = sys_getkbevent()) >= 0) {
            int rel = ev & 0x100, sc = ev & 0x7F, down = !rel;
            switch (sc) {
            case 0x01: sys_setkbmode(0); return 0;
            case 0x48: case 0x11: hu = down; if (down) mu = 1; break;   /* Up / W */
            case 0x4B: case 0x1E: hl = down; if (down) ml = 1; break;   /* Left / A */
            case 0x4D: case 0x20: hr = down; if (down) mr = 1; break;   /* Right / D */
            case 0x13: if (down && over) { RESET(); } break;           /* r restart */
            default: break;
            }
        }
        int thrust_up = (hu || mu), thrust_l = (hl || ml), thrust_r = (hr || mr);

        if (!over) {
            vy += 0.018f;                                  /* gravity */
            if (fuel > 0) {
                if (thrust_up) { vy -= 0.045f; fuel -= 2; }
                if (thrust_l)  { vx -= 0.02f;  fuel -= 1; }   /* Left/A moves left (was += -- backwards from the key) */
                if (thrust_r)  { vx += 0.02f;  fuel -= 1; }   /* Right/D moves right (was -= -- backwards from the key) */
                if (fuel < 0) fuel = 0;
            }
            x += vx; y += vy;
            if (x < 0) { x = 0; vx = 0; } if (x > W - 1) { x = W - 1; vx = 0; }
            if (y < 0) { y = 0; vy = 0; }
            if (y >= GROUND - 6) {                          /* touchdown */
                y = GROUND - 6; over = 1;
                int onpad = (x > PAD0 + 4 && x < PAD1 - 4);
                float av = vy < 0 ? -vy : vy, ah = vx < 0 ? -vx : vx;
                won = onpad && av < SAFE_VY && ah < SAFE_VX;
                if (won) { sys_beep(880, 100); sys_beep(1175, 120); sys_beep(1568, 150); }
                else     { sys_beep(140, 300); }
            }
        }

        /* ---- render ---- */
        for (int i = 0; i < W * H; i++) fb[i] = 0x05060a;           /* night sky */
        for (int s = 0; s < 60; s++) { int sx = (s * 53 + 7) % W, sy = (s * 31 + 3) % (GROUND - 30); fb[sy * W + sx] = 0x335577; }  /* stars */
        line(0, GROUND, PAD0, GROUND - 8, 0x8090a0);                 /* terrain left */
        line(PAD0, GROUND - 8, PAD1, GROUND - 8, 0x22ff44);          /* the landing pad (green) */
        line(PAD1, GROUND - 8, W - 1, GROUND + 4, 0x8090a0);         /* terrain right */
        rect(0, GROUND + 4, W, H - GROUND - 4, 0x101418);            /* ground fill */

        int lx = (int)x, ly = (int)y;
        float av = vy < 0 ? -vy : vy;
        unsigned col = over ? (won ? 0x22ff44 : 0xff3322) : (av < SAFE_VY ? 0xffffff : 0xffcc33);
        line(lx, ly - 5, lx - 4, ly + 4, col);                       /* lander body (a little lander) */
        line(lx, ly - 5, lx + 4, ly + 4, col);
        line(lx - 4, ly + 4, lx + 4, ly + 4, col);
        line(lx - 4, ly + 4, lx - 6, ly + 7, col);                   /* legs */
        line(lx + 4, ly + 4, lx + 6, ly + 7, col);
        if (!over && thrust_up && fuel > 0) line(lx, ly + 5, lx, ly + 10, 0xffaa22);   /* flame */

        rect(6, 6, 104, 8, 0x222a33);                                /* fuel gauge */
        int fw = (int)(fuel / 1000.0f * 100); if (fw < 0) fw = 0; if (fw > 100) fw = 100;
        rect(8, 8, fw, 4, fuel > 300 ? 0x33dd55 : 0xdd4433);

        sys_gfx_blit(fb);
        unsigned long dt = sys_uptime_ms() - t0;
        if (dt < 16) sys_sleep((int)(16 - dt));
    }
}
