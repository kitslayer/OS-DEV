/*
 * asteroids.c — the vector arcade classic, drawn into the framebuffer.
 *
 * Rotate and thrust a little ship around a wrapping screen, blasting asteroids;
 * each big rock you hit splits into two smaller ones, and small ones vanish.
 * Clear the field to spawn a bigger wave. Don't get hit — you have three ships.
 *
 * Left/Right rotate, Up thrusts, Space fires, r restarts, Esc/q quits.
 * Built with SSE (its own Makefile rule) for the float physics + a Taylor
 * sin/cos; input uses the raw make/break queue with a same-frame-tap latch.
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
#define PI 3.14159265f

static float fsin(float x) {
    while (x >  PI) x -= 2*PI;
    while (x < -PI) x += 2*PI;
    float x2=x*x, x3=x*x2, x5=x3*x2, x7=x5*x2;
    return x - x3/6.0f + x5/120.0f - x7/5040.0f;
}
static float fcos(float x) { return fsin(x + PI/2); }

static unsigned rng;
static unsigned rnd(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }
static float frand(void) { return (rnd() & 0xffff) / 65536.0f; }

static unsigned fb[W*H];
static void plot(int x, int y, unsigned c) { if (x>=0 && x<W && y>=0 && y<H) fb[y*W+x] = c; }
static void line(int x0,int y0,int x1,int y1,unsigned c) {
    int dx = x1-x0, dy = y1-y0; dx = dx<0?-dx:dx; dy = dy<0?-dy:dy;
    int sx = x0<x1?1:-1, sy = y0<y1?1:-1, err = dx-dy;
    for (;;) { plot(x0,y0,c); if (x0==x1 && y0==y1) break; int e2 = 2*err; if (e2 > -dy) { err -= dy; x0 += sx; } if (e2 < dx) { err += dx; y0 += sy; } }
}

#define NB 8
#define NA 40
static float shx, shy, sha, svx, svy;
static float bx[NB], by[NB], bvx[NB], bvy[NB]; static int blife[NB];
static float ax[NA], ay[NA], avx[NA], avy[NA]; static int asz[NA], aalive[NA];
static int lives, over, wave;

static float wrapf(float v, float hi) { if (v < 0) v += hi; if (v >= hi) v -= hi; return v; }

static void spawn_wave(int n) {
    for (int i = 0; i < NA; i++) aalive[i] = 0;
    for (int i = 0; i < n && i < NA; i++) {
        aalive[i] = 1; asz[i] = 3;
        ax[i] = frand()*W; ay[i] = frand()*H;
        if (ax[i] > W/2-40 && ax[i] < W/2+40 && ay[i] > H/2-40 && ay[i] < H/2+40) ax[i] = 10;  /* not on the ship */
        float a = frand()*2*PI, sp = 0.4f + frand();
        avx[i] = fcos(a)*sp; avy[i] = fsin(a)*sp;
    }
}

static void reset_ship(void) { shx = W/2; shy = H/2; sha = -PI/2; svx = svy = 0; }
static void reset(void) { lives = 3; over = 0; wave = 1; reset_ship(); for (int i=0;i<NB;i++) blife[i]=0; spawn_wave(4); }

static int arad(int s) { return s==3 ? 16 : s==2 ? 9 : 5; }

static void split(int i) {
    int s = asz[i];
    sys_beep(s==3?180:s==2?300:520, 30);
    if (s > 1) {
        int made = 0;
        for (int k = 0; k < NA && made < 2; k++) if (!aalive[k]) {
            aalive[k]=1; asz[k]=s-1; ax[k]=ax[i]; ay[k]=ay[i];
            float a=frand()*2*PI, sp=0.6f+frand()*1.2f; avx[k]=fcos(a)*sp; avy[k]=fsin(a)*sp; made++;
        }
    }
    aalive[i] = 0;
}

static void draw_ship(float x, float y, float a, unsigned c) {
    float nx = x + fcos(a)*8,        ny = y + fsin(a)*8;             /* nose */
    float lx = x + fcos(a+2.5f)*7,   ly = y + fsin(a+2.5f)*7;
    float rx = x + fcos(a-2.5f)*7,   ry = y + fsin(a-2.5f)*7;
    line((int)nx,(int)ny,(int)lx,(int)ly,c);
    line((int)nx,(int)ny,(int)rx,(int)ry,c);
    line((int)lx,(int)ly,(int)rx,(int)ry,c);
}
static void draw_rock(float x, float y, int r, unsigned c) {
    int px=0, py=0, fx=0, fy=0;
    for (int k = 0; k <= 8; k++) {
        float a = k * (2*PI/8);
        int qx = (int)(x + fcos(a)*r), qy = (int)(y + fsin(a)*r);
        if (k == 0) { fx=qx; fy=qy; } else line(px,py,qx,qy,c);
        px=qx; py=qy;
    }
    line(px,py,fx,fy,c);
}

int main(void) {
    int hl=0, hr=0, ht=0;       /* held: left, right, thrust */
    unsigned long shoot_cd = 0;
    rng = (unsigned)sys_uptime_ms() | 1u;
    if (sys_gfx_init(W,H) < 0) { print("asteroids: gfx init failed\n"); return 1; }
    sys_setkbmode(1);
    reset();

    for (;;) {
        unsigned long now = sys_uptime_ms(), t0 = now;
        int ev, fire=0, ml=0, mr=0, mt=0;
        while ((ev = sys_getkbevent()) >= 0) {
            int rel = ev&0x100, sc = ev&0x7F, down = !rel;
            switch (sc) {
            case 0x01: sys_setkbmode(0); return 0;                  /* Esc quits */
            case 0x4B: case 0x1E: hl = down; if (down) ml=1; break; /* Left / A */
            case 0x4D: case 0x20: hr = down; if (down) mr=1; break; /* Right / D */
            case 0x48: case 0x11: ht = down; if (down) mt=1; break; /* Up / W */
            case 0x39: if (down) fire = 1; break;                   /* space */
            case 0x13: if (down && over) reset(); break;            /* r restart */
            default: break;
            }
        }
        int al = hl||ml, ar = hr||mr, at = ht||mt;

        if (!over) {
            if (al) sha -= 0.13f;
            if (ar) sha += 0.13f;
            if (at) { svx += fcos(sha)*0.18f; svy += fsin(sha)*0.18f; }
            svx *= 0.992f; svy *= 0.992f;
            shx = wrapf(shx+svx, W); shy = wrapf(shy+svy, H);

            if (fire && now >= shoot_cd) {
                for (int i = 0; i < NB; i++) if (!blife[i]) {
                    bx[i]=shx+fcos(sha)*8; by[i]=shy+fsin(sha)*8;
                    bvx[i]=fcos(sha)*4.5f+svx; bvy[i]=fsin(sha)*4.5f+svy; blife[i]=70;
                    shoot_cd = now+160; sys_beep(1400,15); break;
                }
            }
            for (int i = 0; i < NB; i++) if (blife[i]) {
                bx[i]=wrapf(bx[i]+bvx[i],W); by[i]=wrapf(by[i]+bvy[i],H); blife[i]--;
                for (int j = 0; j < NA; j++) if (aalive[j]) {
                    float dx=bx[i]-ax[j], dy=by[i]-ay[j];
                    if (dx*dx+dy*dy < (float)(arad(asz[j])*arad(asz[j]))) { split(j); blife[i]=0; break; }
                }
            }
            int live=0;
            for (int j = 0; j < NA; j++) if (aalive[j]) {
                live++;
                ax[j]=wrapf(ax[j]+avx[j],W); ay[j]=wrapf(ay[j]+avy[j],H);
                float dx=shx-ax[j], dy=shy-ay[j]; int r=arad(asz[j])+6;
                if (dx*dx+dy*dy < (float)(r*r)) {
                    lives--; sys_beep(120,200);
                    if (lives <= 0) { over = 1; }
                    else { reset_ship(); }
                }
            }
            if (live == 0) { wave++; reset_ship(); spawn_wave(3+wave); sys_beep(880,80); sys_beep(1175,80); }
        }

        for (int i = 0; i < W*H; i++) fb[i] = 0x05070d;             /* space */
        for (int j = 0; j < NA; j++) if (aalive[j]) draw_rock(ax[j], ay[j], arad(asz[j]), 0xb0b0c0);
        for (int i = 0; i < NB; i++) if (blife[i]) { plot((int)bx[i],(int)by[i],0xffff66); plot((int)bx[i]+1,(int)by[i],0xffff66); }
        if (!over) draw_ship(shx, shy, sha, at ? 0x66ddff : 0xffffff);
        for (int l = 0; l < lives; l++) draw_ship(12 + l*14, 12, -PI/2, 0x88ff88);   /* lives, top-left */

        sys_gfx_blit(fb);
        unsigned long dt = sys_uptime_ms() - t0;
        if (dt < 16) sys_sleep((int)(16 - dt));
    }
}
