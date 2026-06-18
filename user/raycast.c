/*
 * raycast.c — a from-scratch first-person shooter: pseudo-3D maze (the
 * Wolfenstein 3D rendering trick) drawn against this OS's framebuffer, with
 * billboarded enemy "orbs" you can shoot.
 *
 * Walls: one ray per screen column is marched across a 2D grid (DDA) to the
 * first wall and drawn as a vertical slice of height 1/distance, shaded by
 * distance + side, over a sky/floor split. Each column's wall distance is kept
 * in a depth buffer so sprites behind walls are correctly hidden.
 *
 * Sprites: each living enemy is transformed into camera space, projected to a
 * screen column + size (1/depth), and drawn as a shaded ellipse where it isn't
 * occluded by a nearer wall. Enemies slowly home in on you.
 *
 * Arrows / WASD: up/W forward, down/S back, left/A & right/D turn. Space fires
 * at whatever is under the crosshair. Clear the orbs and reach the green exit.
 * Esc quits.
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
    return x - x3/6.0f + x5/120.0f - x7/5040.0f;
}
static float fcos(float x) { return fsin(x + PI/2); }
static float fabsf2(float x) { return x < 0 ? -x : x; }

static unsigned fb[W * H];
static float zbuf[W];

#define NSPR 6
static float sprX[NSPR], sprY[NSPR];
static int   sprAlive[NSPR];
static int   kills, remaining;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void init_enemies(void) {
    static const float ex[NSPR] = { 7.5f, 12.5f, 2.5f, 13.5f, 5.5f, 9.5f };
    static const float ey[NSPR] = { 7.5f,  2.5f,13.5f,  5.5f, 9.5f,11.5f };
    for (int i = 0; i < NSPR; i++) { sprX[i] = ex[i]; sprY[i] = ey[i]; sprAlive[i] = 1; }
    kills = 0; remaining = NSPR;
}

static void vline(int x, int y0, int y1, unsigned color) {
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) fb[y * W + x] = color;
}

static unsigned shade(unsigned base, float dist, int side) {
    int s = (int)(255.0f / (1.0f + dist * dist * 0.06f));
    if (s > 255) s = 255; if (s < 24) s = 24;
    if (side) s = s * 3 / 4;
    unsigned r = (((base >> 16) & 0xff) * s) >> 8;
    unsigned g = (((base >> 8)  & 0xff) * s) >> 8;
    unsigned b = (( base        & 0xff) * s) >> 8;
    return (r << 16) | (g << 8) | b;
}

int main(void) {
    float posX = 2.5f, posY = 7.5f;
    float dirX = 1.0f, dirY = 0.0f;
    float planeX = 0.0f, planeY = 0.66f;
    int held_fwd = 0, held_back = 0, held_left = 0, held_right = 0;
    int won = 0;
    unsigned long shoot_cd = 0;     /* fire cooldown (ms) */
    rng = (unsigned)sys_uptime_ms() | 1u;
    init_enemies();

    if (sys_gfx_init(W, H) < 0) { print("raycast: gfx init failed\n"); return 1; }
    sys_setkbmode(1);

    for (;;) {
        unsigned long now = sys_uptime_ms(), t0 = now;

        int ev, made_fwd = 0, made_back = 0, made_left = 0, made_right = 0, fire = 0;
        while ((ev = sys_getkbevent()) >= 0) {
            int rel = ev & 0x100, sc = ev & 0x7F;
            int down = !rel;
            switch (sc) {
            case 0x01: sys_setkbmode(0); return 0;
            case 0x48: case 0x11: held_fwd = down;   if (down) made_fwd = 1;   break;
            case 0x50: case 0x1F: held_back = down;  if (down) made_back = 1;  break;
            case 0x4B: case 0x1E: held_left = down;  if (down) made_left = 1;  break;
            case 0x4D: case 0x20: held_right = down; if (down) made_right = 1; break;
            case 0x39: if (down) fire = 1; break;     /* space */
            default: break;
            }
        }
        int af = held_fwd || made_fwd, ab = held_back || made_back;
        int al = held_left || made_left, ar = held_right || made_right;

        if (!won) {
            float move = 0.08f, rot = 0.05f;
            if (af) { float nx = posX+dirX*move, ny = posY+dirY*move; if (!wall_at((int)nx,(int)posY)) posX=nx; if (!wall_at((int)posX,(int)ny)) posY=ny; }
            if (ab) { float nx = posX-dirX*move, ny = posY-dirY*move; if (!wall_at((int)nx,(int)posY)) posX=nx; if (!wall_at((int)posX,(int)ny)) posY=ny; }
            if (al || ar) {
                float a = al ? rot : -rot;
                float odx = dirX, opx = planeX, c = fcos(a), s = fsin(a);
                dirX = dirX*c - dirY*s;   dirY = odx*s + dirY*c;
                planeX = planeX*c - planeY*s; planeY = opx*s + planeY*c;
            }

            /* enemies home in slowly; a touch respawns them across the map */
            for (int i = 0; i < NSPR; i++) {
                if (!sprAlive[i]) continue;
                float dx = posX - sprX[i], dy = posY - sprY[i];
                float d = dx*dx + dy*dy;
                if (d < 0.25f) { sys_beep(150, 60); sprX[i] = 1.5f + (rnd()%13); sprY[i] = 1.5f + (rnd()%13); continue; }
                float step = 0.02f, inv = 1.0f/(d < 0.0001f ? 0.01f : (dx<0?-dx:dx)+(dy<0?-dy:dy));
                float mx = sprX[i] + dx*inv*step, my = sprY[i] + dy*inv*step;
                if (!wall_at((int)mx,(int)sprY[i])) sprX[i] = mx;
                if (!wall_at((int)sprX[i],(int)my)) sprY[i] = my;
            }
            if (MAP[(int)posY][(int)posX] == 'G') { won = 1; sys_beep(880,120); sys_beep(1320,180); }
        }

        /* ---- walls ---- */
        for (int y = 0; y < H/2; y++) for (int x = 0; x < W; x++) fb[y*W+x] = 0x223344;
        for (int y = H/2; y < H; y++) for (int x = 0; x < W; x++) fb[y*W+x] = 0x3a2f25;
        for (int x = 0; x < W; x++) {
            float cameraX = 2.0f*x/W - 1.0f;
            float rdX = dirX + planeX*cameraX, rdY = dirY + planeY*cameraX;
            int mapX = (int)posX, mapY = (int)posY;
            float ddX = rdX == 0 ? 1e30f : (rdX<0 ? -1.0f/rdX : 1.0f/rdX);
            float ddY = rdY == 0 ? 1e30f : (rdY<0 ? -1.0f/rdY : 1.0f/rdY);
            int stepX, stepY, side = 0; float sX, sY;
            if (rdX < 0) { stepX=-1; sX=(posX-mapX)*ddX; } else { stepX=1; sX=(mapX+1.0f-posX)*ddX; }
            if (rdY < 0) { stepY=-1; sY=(posY-mapY)*ddY; } else { stepY=1; sY=(mapY+1.0f-posY)*ddY; }
            int hit = 0; char tile = '#';
            for (int g = 0; g < 64 && !hit; g++) {
                if (sX < sY) { sX+=ddX; mapX+=stepX; side=0; } else { sY+=ddY; mapY+=stepY; side=1; }
                if (mapX<0||mapX>=MW||mapY<0||mapY>=MH) hit=1;
                else if (MAP[mapY][mapX]=='#'||MAP[mapY][mapX]=='G') { hit=1; tile=MAP[mapY][mapX]; }
            }
            float perp = side==0 ? (sX-ddX) : (sY-ddY);
            if (perp < 0.01f) perp = 0.01f;
            zbuf[x] = perp;
            int lh = (int)(H/perp);
            unsigned base = tile=='G' ? 0x22ff44 : ((mapX+mapY)&1) ? 0xb04030 : 0x4060c0;
            vline(x, H/2 - lh/2, H/2 + lh/2, shade(base, perp, side));
        }

        /* ---- enemy sprites (far to near, occluded by zbuf) ---- */
        int order[NSPR]; float sd[NSPR];
        for (int i = 0; i < NSPR; i++) { order[i]=i; float dx=posX-sprX[i],dy=posY-sprY[i]; sd[i]=dx*dx+dy*dy; }
        for (int i = 0; i < NSPR-1; i++) for (int j = i+1; j < NSPR; j++) if (sd[order[j]] > sd[order[i]]) { int t=order[i]; order[i]=order[j]; order[j]=t; }
        for (int o = 0; o < NSPR; o++) {
            int i = order[o]; if (!sprAlive[i]) continue;
            float sx = sprX[i]-posX, sy = sprY[i]-posY;
            float invDet = 1.0f/(planeX*dirY - dirX*planeY);
            float tx = invDet*(dirY*sx - dirX*sy);
            float ty = invDet*(-planeY*sx + planeX*sy);   /* depth */
            if (ty <= 0.1f) continue;
            int scrX = (int)((W/2)*(1.0f + tx/ty));
            int sh = (int)(H/ty); if (sh > H) sh = H;
            int cy = H/2, cx = scrX, rad = sh/2;
            if (rad < 1) continue;
            unsigned col = shade(0xff4422, ty, 0);
            for (int x = cx-rad; x <= cx+rad; x++) {
                if (x < 0 || x >= W) continue;
                if (ty >= zbuf[x]) continue;               /* behind a wall */
                int ex = x - cx;
                for (int y = cy-rad; y <= cy+rad; y++) {
                    if (y < 0 || y >= H) continue;
                    int ey = y - cy;
                    if (ex*ex + ey*ey <= rad*rad) fb[y*W+x] = col;   /* ellipse/orb body */
                }
            }
        }

        /* ---- crosshair ---- */
        for (int d = -4; d <= 4; d++) {
            if (W/2+d >= 0 && W/2+d < W) fb[(H/2)*W + W/2+d] = 0xffffff;
            if (H/2+d >= 0 && H/2+d < H) fb[(H/2+d)*W + W/2] = 0xffffff;
        }

        /* ---- shooting: hit the nearest living enemy under the crosshair ---- */
        if (fire && !won && now >= shoot_cd) {
            shoot_cd = now + 250; sys_beep(1500, 25);
            int best = -1; float bestTy = 1e30f;
            for (int i = 0; i < NSPR; i++) {
                if (!sprAlive[i]) continue;
                float sx = sprX[i]-posX, sy = sprY[i]-posY;
                float invDet = 1.0f/(planeX*dirY - dirX*planeY);
                float tx = invDet*(dirY*sx - dirX*sy);
                float ty = invDet*(-planeY*sx + planeX*sy);
                if (ty <= 0.1f) continue;
                if (fabsf2(tx) > 0.35f*ty) continue;        /* not under the crosshair */
                if (ty >= zbuf[W/2]) continue;              /* a wall is in the way */
                if (ty < bestTy) { bestTy = ty; best = i; }
            }
            if (best >= 0) { sprAlive[best] = 0; kills++; remaining--; sys_beep(700,40); sys_beep(400,60); }
        }

        sys_gfx_blit(fb);
        unsigned long dt = sys_uptime_ms() - t0;
        if (dt < 16) sys_sleep((int)(16 - dt));
    }
}
