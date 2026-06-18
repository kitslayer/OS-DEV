/*
 * missile.c — Missile Command (mouse-driven, in the framebuffer).
 *
 * Defend your six cities from incoming missiles. Move the mouse to aim the
 * crosshair and click to detonate an interceptor there; the expanding blast
 * destroys any missile it touches. A missile that reaches the ground takes out
 * the nearest city. When every city is gone it's over (the sky goes red) —
 * press r to play again, Esc to quit. The waves get faster.
 *
 * A graphics + mouse app: it opens a pixel canvas (sys_gfx_init/blit) and reads
 * the cursor with sys_mouse(). Floating point, so it has its own SSE build rule.
 */
#include "ulib.h"

extern int sys_gfx_init(int w, int h);
extern int sys_gfx_blit(const void *pixels);

#define W 320
#define H 200
#define GROUND 186
#define NCITY 6
#define NM 24
#define NE 10
#define MAXR 18.0f

static unsigned fb[W*H];
static int   city_alive[NCITY];
static int   city_x[NCITY] = { 28, 64, 100, 220, 256, 292 };   /* base sits in the gap at 160 */

static float mx_[NM], my_[NM], mvx[NM], mvy[NM], msx[NM], msy[NM];
static int   m_alive[NM];
static float ecx[NE], ecy[NE], er[NE]; static int e_grow[NE], e_alive[NE];

static int over, wave, killed;
static unsigned long spawn_cd, fire_cd;
static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void plot(int x, int y, unsigned c) { if (x >= 0 && x < W && y >= 0 && y < H) fb[y*W+x] = c; }
static void line(int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1>x0?x1-x0:x0-x1, sx = x0<x1?1:-1;
    int dy = y1>y0?y1-y0:y0-y1, sy = y0<y1?1:-1, err = (dx>dy?dx:-dy)/2;
    for (;;) { plot(x0,y0,c); if (x0==x1 && y0==y1) break; int e2=err; if (e2>-dx){err-=dy;x0+=sx;} if (e2<dy){err+=dx;y0+=sy;} }
}
static void fill_rect(int x, int y, int w, int h, unsigned c) {
    for (int j = y; j < y+h; j++) for (int i = x; i < x+w; i++) plot(i, j, c);
}
static void fill_circle(int cx, int cy, int r, unsigned c) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++)
        if (i*i + j*j <= r*r) plot(cx+i, cy+j, c);
}

static void reset(void) {
    for (int i = 0; i < NCITY; i++) city_alive[i] = 1;
    for (int i = 0; i < NM; i++) m_alive[i] = 0;
    for (int i = 0; i < NE; i++) e_alive[i] = 0;
    over = 0; wave = 1; killed = 0; spawn_cd = 0; fire_cd = 0;
}
static int cities_left(void) { int n = 0; for (int i = 0; i < NCITY; i++) n += city_alive[i]; return n; }

static void spawn_missile(void) {
    int idx = -1; for (int i = 0; i < NM; i++) if (!m_alive[i]) { idx = i; break; }
    if (idx < 0 || cities_left() == 0) return;
    float startx = (float)(rnd() % W);
    /* aim at a surviving city (fall back to anywhere on the ground) */
    int tries = 0, tc; do { tc = (int)(rnd() % NCITY); } while (!city_alive[tc] && ++tries < 16);
    float tx = city_alive[tc] ? (float)(city_x[tc] + 6) : (float)(rnd() % W);
    float dx = tx - startx, dy = (float)GROUND;
    float len = dx*dx + dy*dy; float inv = 1.0f; if (len > 1.0f) { /* crude 1/sqrt via Newton */
        float g = len * 0.5f; for (int k = 0; k < 6; k++) g = 0.5f*(g + len/g); inv = 1.0f/g; }
    float sp = 0.45f + 0.10f * (float)wave;
    mx_[idx] = startx; my_[idx] = 0; msx[idx] = startx; msy[idx] = 0;
    mvx[idx] = dx * inv * sp; mvy[idx] = dy * inv * sp; m_alive[idx] = 1;
}
static void fire(float x, float y) {
    int idx = -1; for (int i = 0; i < NE; i++) if (!e_alive[i]) { idx = i; break; }
    if (idx < 0) return;
    ecx[idx] = x; ecy[idx] = y; er[idx] = 1.0f; e_grow[idx] = 1; e_alive[idx] = 1;
    sys_beep(1200, 20);
}
static void hit_nearest_city(float x) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < NCITY; i++) if (city_alive[i]) { float d = (float)city_x[i]+6 - x; if (d<0)d=-d; if (d<bd){bd=d;best=i;} }
    if (best >= 0) { city_alive[best] = 0; sys_beep(90, 220); if (cities_left()==0){ over=1; sys_beep(70,500); } }
}

static void render(void) {
    unsigned sky = over ? 0x3a0808 : 0x0a0a20;
    for (int i = 0; i < W*H; i++) fb[i] = sky;
    fill_rect(0, GROUND, W, H-GROUND, 0x244018);                 /* ground */
    for (int i = 0; i < NCITY; i++)                              /* cities */
        if (city_alive[i]) { fill_rect(city_x[i], GROUND-8, 12, 8, 0x4488dd); fill_rect(city_x[i]+3, GROUND-12, 6, 4, 0x66aaff); }
        else fill_rect(city_x[i], GROUND-3, 12, 3, 0x403030);     /* rubble */
    fill_rect(155, GROUND-10, 10, 10, 0xc0c0c0);                 /* the battery */
    for (int i = 0; i < NM; i++) if (m_alive[i]) {               /* missiles + trails */
        line((int)msx[i], (int)msy[i], (int)mx_[i], (int)my_[i], 0x802020);
        plot((int)mx_[i], (int)my_[i], 0xff5050); plot((int)mx_[i]+1, (int)my_[i], 0xff8080);
    }
    for (int i = 0; i < NE; i++) if (e_alive[i])                 /* blasts */
        fill_circle((int)ecx[i], (int)ecy[i], (int)er[i], e_grow[i] ? 0xffffa0 : 0xffa030);
    for (int i = 0; i < wave && i < 30; i++) plot(4+i*3, 4, 0x70ff70);   /* wave gauge */
    for (int i = 0; i < killed && i < 96; i++) plot(4 + i*3, 9, 0xffe060); /* kills gauge */
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    if (sys_gfx_init(W, H) < 0) { print("missile: gfx init failed\n"); return 1; }
    sys_setkbmode(1);
    reset();
    int prevdown = 0;
    for (;;) {
        unsigned long now = sys_uptime_ms(), t0 = now;
        int ev;
        while ((ev = sys_getkbevent()) >= 0) {
            int sc = ev & 0x7F, down = !(ev & 0x100);
            if (sc == 0x01 && down) { sys_setkbmode(0); return 0; }      /* Esc */
            if (sc == 0x13 && down && over) reset();                     /* r restart */
        }
        int mxp, myp, b = sys_mouse(&mxp, &myp);
        int down = (mxp >= 0) && (b & 1);
        if (!over && down && !prevdown && now >= fire_cd) { fire((float)mxp, (float)myp); fire_cd = now + 220; }
        prevdown = down;

        if (!over) {
            if (now >= spawn_cd) { spawn_missile(); spawn_cd = now + (unsigned long)(1400 - wave*60 > 350 ? 1400 - wave*60 : 350); }
            for (int i = 0; i < NE; i++) if (e_alive[i]) {              /* grow/shrink blasts */
                if (e_grow[i]) { er[i] += 1.3f; if (er[i] >= MAXR) e_grow[i] = 0; }
                else { er[i] -= 0.4f; if (er[i] <= 0) e_alive[i] = 0; }   /* linger so missiles fly into it */
            }
            int live = 0;
            for (int i = 0; i < NM; i++) if (m_alive[i]) {             /* move missiles */
                live++;
                mx_[i] += mvx[i]; my_[i] += mvy[i];
                for (int e = 0; e < NE; e++) if (e_alive[e]) {         /* intercepted? */
                    float dx = mx_[i]-ecx[e], dy = my_[i]-ecy[e];
                    if (dx*dx + dy*dy <= er[e]*er[e]) { m_alive[i] = 0; killed++; sys_beep(1600, 15); break; }
                }
                if (m_alive[i] && my_[i] >= (float)GROUND) { m_alive[i] = 0; hit_nearest_city(mx_[i]); }
            }
            if (live == 0 && now >= spawn_cd - 200) { /* a lull -> next wave ramps difficulty */ }
            static unsigned long wave_cd = 0;
            if (now >= wave_cd) { wave++; wave_cd = now + 18000; }      /* ramp up every 18s */
        }

        render();
        if (mxp >= 0) {                                                 /* crosshair */
            line(mxp-4, myp, mxp+4, myp, 0xffffff); line(mxp, myp-4, mxp, myp+4, 0xffffff);
        }
        sys_gfx_blit(fb);
        unsigned long dt = sys_uptime_ms() - t0;
        if (dt < 16) sys_sleep((int)(16 - dt));
    }
}
