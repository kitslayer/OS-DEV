/*
 * demoscene.c — "insane graphics": a real-time, GPU-less demoscene for OS-DEV.
 *
 * Everything here is software-rendered into a malloc'd 0x00RRGGBB framebuffer and
 * pushed to the window with sys_gfx_blit — no GPU, no floating point. The kernel
 * builds userspace with -mgeneral-regs-only, so ALL the math is fixed-point: a
 * sine lookup table (built at startup with Bhaskara's integer approximation, no
 * libm), an integer square root, and an integer atan2. On that base it runs the
 * classic effects, cycled with cross-fades and overlaid with a sine-wobble
 * scroller:
 *
 *   plasma · rotozoom tunnel · fire · 3D flat-shaded icosahedron ·
 *   starfield · metaballs · animated Julia set
 *
 * Controls: SPACE = next effect, 1..8 = jump to an effect, Q/Esc = quit.
 */
#include "ulib.h"

#define W   480
#define H   360
#define HW  (W / 2)
#define HH  (H / 2)

/* ---- fixed-point trig / math (no FPU) ------------------------------------ */
/* Angles are 0..1023 around the circle; isin/icos return Q12 (-4096..4096). */
static int SINTAB[1024];

static void build_tables(void) {
    /* Bhaskara I: sin(d deg) ~= 4d(180-d) / (40500 - d(180-d)), d in [0,180]. */
    for (int i = 0; i < 1024; i++) {
        long d = (long)i * 360 / 1024;     /* degrees 0..359 */
        int neg = 0;
        if (d >= 180) { d -= 180; neg = 1; }
        long t = d * (180 - d);
        long s = (4 * t * 4096) / (40500 - t);   /* Q12 */
        SINTAB[i] = neg ? (int)(-s) : (int)s;
    }
}
static inline int isin(int a) { return SINTAB[a & 1023]; }
static inline int icos(int a) { return SINTAB[(a + 256) & 1023]; }

static unsigned isqrt(unsigned long v) {
    unsigned long r = 0, b = 1UL << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (unsigned)r;
}

/* Integer atan2 -> 0..1023 (monotonic; the per-octant term is a smooth linear
 * stand-in for atan, which is all a texture-wrap needs). */
static int iatan2(int y, int x) {
    if (!x && !y) return 0;
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a;
    if (ax >= ay) a = (ay * 128) / ax;            /* 0..128 */
    else          a = 256 - (ax * 128) / ay;      /* 128..256 */
    if (x >= 0 && y >= 0) return a;               /* Q1   0..256  */
    if (x <  0 && y >= 0) return 512 - a;         /* Q2 256..512  */
    if (x <  0 && y <  0) return 512 + a;         /* Q3 512..768  */
    return 1024 - a;                              /* Q4 768..1024 */
}

static unsigned rng = 0x1234abcdu;
static inline unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline unsigned rgb(int r, int g, int b) {
    return ((unsigned)clamp8(r) << 16) | ((unsigned)clamp8(g) << 8) | (unsigned)clamp8(b);
}

static unsigned *fb;                  /* the W*H frame we blit each frame */

/* ---- effect 1: plasma ----------------------------------------------------- */
static void fx_plasma(int t) {
    for (int y = 0; y < H; y++) {
        int ry = isin(y * 3 + t) + isin(y - t * 2);
        for (int x = 0; x < W; x++) {
            int v = isin(x * 3 + t) + ry
                  + isin(((x + y) * 2 + t))
                  + isin((isqrt((unsigned)((x-HW)*(x-HW) + (y-HH)*(y-HH)))) * 6 - t * 3);
            int i = (v >> 5) + t;        /* palette index, animated */
            fb[y * W + x] = rgb(128 + isin(i) / 32,
                                128 + isin(i + 341) / 32,
                                128 + isin(i + 682) / 32);
        }
    }
}

/* ---- effect 2: rotozoom tunnel -------------------------------------------- */
static int *TANG, *TDEP;              /* precomputed per-pixel angle + depth */
static void tunnel_init(void) {
    TANG = malloc((unsigned long)W * H * sizeof(int));
    TDEP = malloc((unsigned long)W * H * sizeof(int));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int dx = x - HW, dy = y - HH;
            unsigned r = isqrt((unsigned)(dx * dx + dy * dy)) + 1;
            TANG[y * W + x] = (iatan2(dy, dx) * 256) / 1024;   /* 0..255 around  */
            TDEP[y * W + x] = (int)(8192u / r);                /* far->small     */
        }
}
static void fx_tunnel(int t) {
    for (int y = 0; y < H; y++) {
        int row = y * W;
        for (int x = 0; x < W; x++) {
            int u = (TANG[row + x] + (t >> 1)) & 255;
            int v = (TDEP[row + x] + t) & 255;
            int tex = ((u ^ v) & 64) ? 255 : 60;          /* checker texture */
            int sh = TDEP[row + x]; if (sh > 255) sh = 255;  /* depth shade */
            int b = (tex * sh) >> 8;
            fb[row + x] = rgb(b, (b * 3) >> 2, b >> 1);    /* warm tunnel */
        }
    }
}

/* ---- effect 3: fire ------------------------------------------------------- */
static unsigned char *fire;
static unsigned firepal[256];
static void fire_init(void) {
    fire = malloc((unsigned long)W * H);
    for (int i = 0; i < (int)((unsigned long)W * H); i++) fire[i] = 0;
    for (int i = 0; i < 256; i++) {                       /* black->red->yellow->white */
        int r = clamp8(i * 3), g = clamp8((i - 80) * 3), b = clamp8((i - 180) * 4);
        firepal[i] = rgb(r, g, b);
    }
}
static void fx_fire(int t) {
    (void)t;
    for (int x = 0; x < W; x++)                            /* stoke the bottom rows */
        for (int yy = 0; yy < 2; yy++)
            fire[(H - 1 - yy) * W + x] = (rnd() & 1) ? 255 : 0;
    for (int y = 0; y < H - 2; y++)
        for (int x = 0; x < W; x++) {
            int xl = x ? x - 1 : 0, xr = x < W - 1 ? x + 1 : W - 1;
            int s = fire[(y + 1) * W + xl] + fire[(y + 1) * W + x]
                  + fire[(y + 1) * W + xr] + fire[(y + 2) * W + x];
            int v = (s * 13) >> 6;                         /* ~/4 minus a little = cooling */
            fire[y * W + x] = (unsigned char)(v > 255 ? 255 : v);
        }
    for (int i = 0; i < (int)((unsigned long)W * H); i++) fb[i] = firepal[fire[i]];
}

/* ---- effect 4: 3D flat-shaded icosahedron --------------------------------- */
static const int ICOV[12][3] = {
    {-1000, 1618, 0}, {1000, 1618, 0}, {-1000,-1618, 0}, {1000,-1618, 0},
    {0,-1000, 1618}, {0, 1000, 1618}, {0,-1000,-1618}, {0, 1000,-1618},
    {1618, 0,-1000}, {1618, 0, 1000}, {-1618, 0,-1000}, {-1618, 0, 1000},
};
static const int ICOF[20][3] = {
    {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
    {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
    {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
    {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1},
};

static void fill_tri(int x0,int y0,int x1,int y1,int x2,int y2,unsigned col){
    if (y1 < y0){int t;t=y0;y0=y1;y1=t;t=x0;x0=x1;x1=t;}
    if (y2 < y0){int t;t=y0;y0=y2;y2=t;t=x0;x0=x2;x2=t;}
    if (y2 < y1){int t;t=y1;y1=y2;y2=t;t=x1;x1=x2;x2=t;}
    if (y2 == y0) return;
    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= H) continue;
        int xa, xb;
        /* long edge x0->x2 */
        xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        if (y < y1) xb = (y1==y0)?x0 : x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else        xb = (y2==y1)?x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        if (xa > xb){int t=xa;xa=xb;xb=t;}
        if (xa < 0) xa = 0; if (xb >= W) xb = W - 1;
        unsigned *p = fb + y * W;
        for (int x = xa; x <= xb; x++) p[x] = col;
    }
}
static void fx_ico(int t) {
    for (int i = 0; i < (int)((unsigned long)W*H); i++) fb[i] = rgb(6,8,20);  /* deep blue bg */
    int ax = t, ay = (t * 3) / 4;
    int rx[12], ry[12], rz[12], sx[12], sy[12];
    for (int i = 0; i < 12; i++) {
        int x = ICOV[i][0], y = ICOV[i][1], z = ICOV[i][2];
        /* rotate Y then X (Q12) */
        int x1 = (x * icos(ay) - z * isin(ay)) >> 12;
        int z1 = (x * isin(ay) + z * icos(ay)) >> 12;
        int y2 = (y * icos(ax) - z1 * isin(ax)) >> 12;
        int z2 = (y * isin(ax) + z1 * icos(ax)) >> 12;
        rx[i] = x1; ry[i] = y2; rz[i] = z2;
        int zc = z2 + 5200;                       /* push away from camera */
        sx[i] = HW + (x1 * 1400) / zc;
        sy[i] = HH + (y2 * 1400) / zc;
    }
    /* draw faces back-to-front (sort by avg z) with backface cull + flat shade */
    int order[20]; long zavg[20];
    for (int f = 0; f < 20; f++) {
        order[f] = f;
        zavg[f] = rz[ICOF[f][0]] + rz[ICOF[f][1]] + rz[ICOF[f][2]];
    }
    for (int i = 0; i < 20; i++)                  /* simple insertion sort, far first */
        for (int j = i + 1; j < 20; j++)
            if (zavg[order[j]] < zavg[order[i]]) { int tmp=order[i];order[i]=order[j];order[j]=tmp; }
    for (int o = 0; o < 20; o++) {
        int f = order[o];
        int a = ICOF[f][0], b = ICOF[f][1], c = ICOF[f][2];
        /* screen-space cross product -> winding (backface cull) */
        long cross = (long)(sx[b]-sx[a])*(sy[c]-sy[a]) - (long)(sy[b]-sy[a])*(sx[c]-sx[a]);
        if (cross <= 0) continue;
        /* flat shade: face normal . light, in object space */
        int ux=rx[b]-rx[a], uy=ry[b]-ry[a], uz=rz[b]-rz[a];
        int vx=rx[c]-rx[a], vy=ry[c]-ry[a], vz=rz[c]-rz[a];
        int nx=(uy*vz-uz*vy)>>10, ny=(uz*vx-ux*vz)>>10, nz=(ux*vy-uy*vx)>>10;
        unsigned nl = isqrt((unsigned)(nx*nx+ny*ny+nz*nz)) + 1;
        int d = ((nx + ny - nz) * 200) / (int)nl;   /* light from (1,1,-1) */
        if (d < 0) d = 0; d += 40;
        unsigned col = rgb((d * (f * 11 + 60)) >> 8, (d * 200) >> 8, (d * (255 - f * 6)) >> 8);
        fill_tri(sx[a],sy[a], sx[b],sy[b], sx[c],sy[c], col);
    }
}

/* ---- effect 5: starfield -------------------------------------------------- */
#define NSTAR 360
static int starx[NSTAR], stary[NSTAR], starz[NSTAR];
static void stars_init(void) {
    for (int i = 0; i < NSTAR; i++) {
        starx[i] = (int)(rnd() % 4000) - 2000;
        stary[i] = (int)(rnd() % 4000) - 2000;
        starz[i] = (int)(rnd() % 2000) + 1;
    }
}
static void fx_stars(int t) {
    (void)t;
    for (int i = 0; i < (int)((unsigned long)W*H); i++) fb[i] = 0;
    for (int i = 0; i < NSTAR; i++) {
        starz[i] -= 30;
        if (starz[i] <= 0) { starx[i]=(int)(rnd()%4000)-2000; stary[i]=(int)(rnd()%4000)-2000; starz[i]=2000; }
        int sx = HW + starx[i] * 400 / starz[i];
        int sy = HH + stary[i] * 400 / starz[i];
        if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
        int b = 255 - starz[i] * 255 / 2000;
        unsigned c = rgb(b, b, clamp8(b + 40));
        fb[sy * W + sx] = c;
        if (starz[i] < 700) {                       /* near stars: a little bigger */
            if (sx + 1 < W) fb[sy * W + sx + 1] = c;
            if (sy + 1 < H) fb[(sy + 1) * W + sx] = c;
        }
    }
}

/* ---- effect 6: metaballs (half-res, 2x2 upscaled) ------------------------- */
#define MB 6
static void fx_meta(int t) {
    int bx[MB], by[MB];
    for (int i = 0; i < MB; i++) {
        bx[i] = HW + (icos(t * (i + 1) + i * 120) * (HW - 40)) / 4096;
        by[i] = HH + (isin(t * (i + 2) + i * 200) * (HH - 40)) / 4096;
    }
    for (int y = 0; y < H; y += 2) {
        for (int x = 0; x < W; x += 2) {
            int field = 0;
            for (int i = 0; i < MB; i++) {
                int dx = x - bx[i], dy = y - by[i];
                field += 240000 / (dx * dx + dy * dy + 80);
            }
            int v = field;
            unsigned c;
            if (v < 90) c = rgb(v / 3, 0, v);                 /* outer glow violet */
            else        c = rgb(clamp8(v), clamp8((v-90)*2), clamp8((v-160)*3)); /* hot core */
            unsigned *p = fb + y * W + x;
            p[0] = c; if (x+1<W) p[1]=c;
            if (y+1<H){ p[W]=c; if(x+1<W) p[W+1]=c; }
        }
    }
}

/* ---- effect 7: animated Julia set (half-res) ------------------------------ */
static void fx_julia(int t) {
    int cx = (icos(t) * 7) / 10;          /* c orbits a circle, Q12 */
    int cy = (isin(t + 90) * 7) / 10;
    for (int y = 0; y < H; y += 2) {
        for (int x = 0; x < W; x += 2) {
            int zx = ((x - HW) * 12) >> 4;     /* ~ -1.5..1.5 in Q12 */
            int zy = ((y - HH) * 12) >> 4;
            int it = 0;
            for (; it < 48; it++) {
                long xx = (long)zx * zx >> 12;
                long yy = (long)zy * zy >> 12;
                if (xx + yy > (4 << 12)) break;
                int nx = (int)(xx - yy) + cx;
                zy = (int)(((long)zx * zy >> 11)) + cy;   /* 2*zx*zy */
                zx = nx;
            }
            int i = it * 5 + t;
            unsigned c = (it >= 48) ? 0 : rgb(128 + isin(i)/32, 128 + isin(i+200)/32, 128 + isin(i+400)/32);
            unsigned *p = fb + y * W + x;
            p[0]=c; if(x+1<W)p[1]=c;
            if(y+1<H){ p[W]=c; if(x+1<W)p[W+1]=c; }
        }
    }
}

/* ---- sine-wobble scroller (a compact 8x8 uppercase font) ------------------ */
/* Supported glyphs, in this order; each is 8 rows (MSB = leftmost of 8 px). */
static const char GLYPHS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-!.:";
static const unsigned char FONT[][8] = {
 {0,0,0,0,0,0,0,0},                                  /* space */
 {0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x00}, /*A*/ {0x7C,0x42,0x7C,0x42,0x42,0x42,0x7C,0x00}, /*B*/
 {0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00}, /*C*/ {0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00}, /*D*/
 {0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00}, /*E*/ {0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00}, /*F*/
 {0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00}, /*G*/ {0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00}, /*H*/
 {0x3E,0x08,0x08,0x08,0x08,0x08,0x3E,0x00}, /*I*/ {0x1E,0x04,0x04,0x04,0x44,0x44,0x38,0x00}, /*J*/
 {0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00}, /*K*/ {0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00}, /*L*/
 {0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x00}, /*M*/ {0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00}, /*N*/
 {0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, /*O*/ {0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00}, /*P*/
 {0x3C,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00}, /*Q*/ {0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00}, /*R*/
 {0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00}, /*S*/ {0x7F,0x08,0x08,0x08,0x08,0x08,0x08,0x00}, /*T*/
 {0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, /*U*/ {0x42,0x42,0x42,0x42,0x42,0x24,0x18,0x00}, /*V*/
 {0x42,0x42,0x42,0x5A,0x5A,0x66,0x42,0x00}, /*W*/ {0x42,0x24,0x18,0x18,0x18,0x24,0x42,0x00}, /*X*/
 {0x41,0x22,0x14,0x08,0x08,0x08,0x08,0x00}, /*Y*/ {0x7E,0x04,0x08,0x10,0x20,0x40,0x7E,0x00}, /*Z*/
 {0x3C,0x46,0x4A,0x52,0x62,0x42,0x3C,0x00}, /*0*/ {0x08,0x18,0x08,0x08,0x08,0x08,0x3E,0x00}, /*1*/
 {0x3C,0x42,0x02,0x0C,0x30,0x40,0x7E,0x00}, /*2*/ {0x3C,0x42,0x02,0x1C,0x02,0x42,0x3C,0x00}, /*3*/
 {0x04,0x0C,0x14,0x24,0x7E,0x04,0x04,0x00}, /*4*/ {0x7E,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00}, /*5*/
 {0x1C,0x20,0x40,0x7C,0x42,0x42,0x3C,0x00}, /*6*/ {0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x00}, /*7*/
 {0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00}, /*8*/ {0x3C,0x42,0x42,0x3E,0x02,0x04,0x38,0x00}, /*9*/
 {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /*-*/ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /*!*/
 {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /*.*/ {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, /*:*/
};
static const char *MSG =
  "OS-DEV DEMOSCENE   100 PERCENT SOFTWARE RENDERED   NO GPU   NO FLOATING POINT   "
  "JUST FIXED-POINT MATH ON A RAW FRAMEBUFFER   PLASMA - TUNNEL - FIRE - 3D - "
  "STARFIELD - METABALLS - JULIA   GREETINGS FROM A KERNEL BUILT ENTIRELY FROM "
  "SCRATCH   PRESS SPACE FOR THE NEXT EFFECT   ENJOY THE SHOW    ";

static int glyph_index(char c) {
    for (int i = 0; GLYPHS[i]; i++) if (GLYPHS[i] == c) return i;
    return 0;
}
static void draw_scroller(int t) {
    int msglen = 0; while (MSG[msglen]) msglen++;
    int scale = 4;                                  /* 8px glyph -> 32px tall */
    int gw = 8 * scale + scale;                     /* glyph advance */
    int total = msglen * gw;
    int scroll = (t * 5) % total;                   /* pixels scrolled */
    int basey = H - 8 * scale - 16;
    for (int sxp = -gw; sxp < W; sxp++) {           /* column-by-column for the wobble */
        int worldx = sxp + scroll;
        int gi = (worldx / gw) % msglen; if (gi < 0) gi += msglen;
        int col = (worldx % gw) / scale;            /* 0..8 within the glyph */
        if (col >= 8) continue;                      /* the inter-glyph gap */
        const unsigned char *gl = FONT[glyph_index(MSG[gi])];
        int wob = isin(sxp * 8 + t * 6) * 28 >> 12;  /* vertical sine wobble */
        for (int gy = 0; gy < 8; gy++) {
            if (!(gl[gy] & (0x80 >> col))) continue;
            int py = basey + gy * scale + wob;
            unsigned cc = rgb(255, 220 - (gy * 12), 80 + gy * 16);
            for (int yy = 0; yy < scale; yy++) {
                int Y = py + yy;
                if (Y < 0 || Y >= H || sxp < 0 || sxp >= W) continue;
                fb[Y * W + sxp] = cc;
            }
        }
    }
}

/* ---- fade-to-black for cross-fades --------------------------------------- */
static void fade(int amt /*0..256*/) {
    for (int i = 0; i < (int)((unsigned long)W * H); i++) {
        unsigned p = fb[i];
        int r = ((int)((p >> 16) & 255) * amt) >> 8;
        int g = ((int)((p >> 8) & 255) * amt) >> 8;
        int b = ((int)(p & 255) * amt) >> 8;
        fb[i] = rgb(r, g, b);
    }
}

/* ---- sequencer ------------------------------------------------------------ */
typedef void (*effect_fn)(int t);
static effect_fn EFFECTS[] = { fx_plasma, fx_tunnel, fx_fire, fx_ico, fx_stars, fx_meta, fx_julia };
#define NFX ((int)(sizeof(EFFECTS) / sizeof(EFFECTS[0])))
#define DUR_MS 11000                  /* time on each effect */
#define FADE_MS 700                   /* cross-fade window at each end */

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("demoscene: graphics init failed\n"); return 1; }
    fb = malloc((unsigned long)W * H * 4);
    if (!fb) { print("demoscene: out of memory\n"); return 1; }
    sys_caret(0);
    build_tables();
    tunnel_init();
    fire_init();
    stars_init();

    unsigned long start = sys_uptime_ms();
    int cur = 0;
    unsigned long fxstart = start;

    for (;;) {
        int k = sys_pollkey();
        if (k == 'q' || k == 'Q' || k == 27) break;
        if (k == ' ') { cur = (cur + 1) % NFX; fxstart = sys_uptime_ms(); }
        if (k >= '1' && k <= '0' + NFX) { cur = k - '1'; fxstart = sys_uptime_ms(); }

        unsigned long now = sys_uptime_ms();
        int local = (int)(now - fxstart);
        if (local >= DUR_MS) { cur = (cur + 1) % NFX; fxstart = now; local = 0; }

        int t = (int)((now - start) / 24);     /* global animation clock */
        EFFECTS[cur](t);

        if (local < FADE_MS)                    fade((local * 256) / FADE_MS);          /* fade in */
        else if (local > DUR_MS - FADE_MS)      fade(((DUR_MS - local) * 256) / FADE_MS); /* fade out */

        draw_scroller(t);
        sys_gfx_blit(fb);
        sys_sleep(20);                          /* aim ~ smooth; effects pace themselves */
    }
    free(fb);
    return 0;
}
