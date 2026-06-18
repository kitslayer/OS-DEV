/*
 * raycast.c — a from-scratch raycaster: pseudo-3D maze, the Wolfenstein 3D
 * rendering trick, written against this OS's framebuffer (no external engine).
 *
 * For each screen column we shoot a ray across a 2D grid (DDA), find the first
 * wall it hits, and draw a vertical slice whose height is inversely proportional
 * to the distance — near walls tower, far walls shrink, giving depth. Walls are
 * coloured by type and shaded by distance and which side was hit. Find the green
 * exit (G) to escape the maze.
 *
 * Arrows or WASD: up/W forward, down/S back, left/A and right/D turn. Esc quits.
 */

/* syscalls from ulib (resolved at link; this file is built with SSE so we can
 * use float math for the ray geometry — see the Makefile's raycast rule). */
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
#define MW 16
#define MH 16

static const char *MAP[MH] = {
    "################",
    "#..............#",
    "#..##..####..#.#",
    "#..#......#..#.#",
    "#..#..##..#....#",
    "#.....#.....##.#",
    "#..####..#..#..#",
    "#........#.....#",
    "#..##..###..##.#",
    "#...#.......#..#",
    "#.#.#.####..#..#",
    "#.#........#...#",
    "#.####..##....G#",
    "#..........#...#",
    "#..##..##..#...#",
    "################",
};
static int wall_at(int x, int y) {
    if (x < 0 || x >= MW || y < 0 || y >= MH) return 1;
    return MAP[y][x] == '#';
}

#define PI 3.14159265f
static float fsin(float x) {
    while (x >  PI) x -= 2*PI;
    while (x < -PI) x += 2*PI;
    float x2 = x*x, x3 = x*x2, x5 = x3*x2, x7 = x5*x2;
    return x - x3/6.0f + x5/120.0f - x7/5040.0f;   /* Taylor; ample for rendering */
}
static float fcos(float x) { return fsin(x + PI/2); }

static unsigned fb[W * H];

static void vline(int x, int y0, int y1, unsigned color) {
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) fb[y * W + x] = color;
}

static unsigned shade(unsigned base, float dist, int side) {
    int s = (int)(255.0f / (1.0f + dist * dist * 0.06f));   /* darken with distance */
    if (s > 255) s = 255; if (s < 24) s = 24;
    if (side) s = s * 3 / 4;                                 /* y-side walls a touch darker */
    unsigned r = (((base >> 16) & 0xff) * s) >> 8;
    unsigned g = (((base >> 8)  & 0xff) * s) >> 8;
    unsigned b = (( base        & 0xff) * s) >> 8;
    return (r << 16) | (g << 8) | b;
}

int main(void) {
    float posX = 2.5f, posY = 7.5f;          /* start in an open corridor, facing down it */
    float dirX = 1.0f, dirY = 0.0f;
    float planeX = 0.0f, planeY = 0.66f;     /* ~66 degree FOV */
    int held_fwd = 0, held_back = 0, held_left = 0, held_right = 0;
    int won = 0;

    if (sys_gfx_init(W, H) < 0) { print("raycast: gfx init failed\n"); return 1; }
    sys_setkbmode(1);

    for (;;) {
        unsigned long t0 = sys_uptime_ms();

        /* Track held keys; also latch a key that was pressed this frame even if
         * its release arrived in the same drain (a synthetic/quick tap), so a
         * tap still moves one frame while a real hold moves continuously. */
        int ev, made_fwd = 0, made_back = 0, made_left = 0, made_right = 0;
        while ((ev = sys_getkbevent()) >= 0) {
            int rel = ev & 0x100, sc = ev & 0x7F;
            int down = !rel;
            switch (sc) {
            case 0x01: sys_setkbmode(0); return 0;                                   /* Esc */
            case 0x48: case 0x11: held_fwd = down;   if (down) made_fwd = 1;   break; /* Up / W */
            case 0x50: case 0x1F: held_back = down;  if (down) made_back = 1;  break; /* Down / S */
            case 0x4B: case 0x1E: held_left = down;  if (down) made_left = 1;  break; /* Left / A */
            case 0x4D: case 0x20: held_right = down; if (down) made_right = 1; break; /* Right / D */
            default: break;
            }
        }
        int af = held_fwd || made_fwd, ab = held_back || made_back;
        int al = held_left || made_left, ar = held_right || made_right;

        if (!won) {
            float move = 0.08f, rot = 0.05f;
            if (af) {
                float nx = posX + dirX * move, ny = posY + dirY * move;
                if (!wall_at((int)nx, (int)posY)) posX = nx;
                if (!wall_at((int)posX, (int)ny)) posY = ny;
            }
            if (ab) {
                float nx = posX - dirX * move, ny = posY - dirY * move;
                if (!wall_at((int)nx, (int)posY)) posX = nx;
                if (!wall_at((int)posX, (int)ny)) posY = ny;
            }
            if (al || ar) {
                float a = al ? rot : -rot;
                float odx = dirX, opx = planeX, c = fcos(a), s = fsin(a);
                dirX = dirX * c - dirY * s;   dirY = odx * s + dirY * c;
                planeX = planeX * c - planeY * s; planeY = opx * s + planeY * c;
            }
            if (MAP[(int)posY][(int)posX] == 'G') { won = 1; sys_beep(880, 120); sys_beep(1320, 180); }
        }

        /* ceiling + floor */
        for (int y = 0; y < H/2; y++) for (int x = 0; x < W; x++) fb[y*W+x] = 0x223344;       /* sky */
        for (int y = H/2; y < H; y++) for (int x = 0; x < W; x++) fb[y*W+x] = 0x3a2f25;       /* ground */

        /* cast one ray per column */
        for (int x = 0; x < W; x++) {
            float cameraX = 2.0f * x / W - 1.0f;
            float rdX = dirX + planeX * cameraX;
            float rdY = dirY + planeY * cameraX;
            int mapX = (int)posX, mapY = (int)posY;
            float ddX = rdX == 0 ? 1e30f : (rdX < 0 ? -1.0f/rdX : 1.0f/rdX);
            float ddY = rdY == 0 ? 1e30f : (rdY < 0 ? -1.0f/rdY : 1.0f/rdY);
            int stepX, stepY, side = 0;
            float sX, sY;
            if (rdX < 0) { stepX = -1; sX = (posX - mapX) * ddX; } else { stepX = 1; sX = (mapX + 1.0f - posX) * ddX; }
            if (rdY < 0) { stepY = -1; sY = (posY - mapY) * ddY; } else { stepY = 1; sY = (mapY + 1.0f - posY) * ddY; }
            int hit = 0; char tile = '#';
            for (int guard = 0; guard < 64 && !hit; guard++) {
                if (sX < sY) { sX += ddX; mapX += stepX; side = 0; }
                else         { sY += ddY; mapY += stepY; side = 1; }
                if (mapX < 0 || mapX >= MW || mapY < 0 || mapY >= MH) { hit = 1; }
                else if (MAP[mapY][mapX] == '#' || MAP[mapY][mapX] == 'G') { hit = 1; tile = MAP[mapY][mapX]; }
            }
            float perp = side == 0 ? (sX - ddX) : (sY - ddY);
            if (perp < 0.01f) perp = 0.01f;
            int lh = (int)(H / perp);
            int y0 = H/2 - lh/2, y1 = H/2 + lh/2;
            unsigned base = tile == 'G' ? 0x22ff44 :          /* the exit glows green */
                            ((mapX + mapY) & 1) ? 0xb04030 : 0x4060c0;
            vline(x, y0, y1, shade(base, perp, side));
        }

        sys_gfx_blit(fb);
        unsigned long dt = sys_uptime_ms() - t0;
        if (dt < 16) sys_sleep((int)(16 - dt));
    }
}
