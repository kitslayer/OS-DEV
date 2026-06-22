/*
 * scene3d.c — a real-time software 3D engine for OS-DEV. No GPU.
 *
 * A from-scratch perspective 3D renderer: procedurally-generated triangle meshes
 * (UV sphere, torus, cube), transformed by a rotation/camera matrix, lit by a
 * directional light (+ ambient + a rim fill light), and rasterized with a
 * per-pixel Z-BUFFER and GOURAUD shading (per-vertex colour interpolated across
 * each triangle, so the sphere/torus shade smoothly). Built with SSE so the
 * geometry/lighting math is plain float (the generic user rule is
 * -mgeneral-regs-only); trig/sqrt are tiny self-contained approximations (no
 * libm), exactly like the raycaster.
 *
 * Controls: SPACE = next model · arrows = orbit the camera · +/- = zoom ·
 * R = toggle auto-spin · Q/Esc = quit.
 */
#include "ulib.h"

#define W 480
#define H 360

#define PI 3.14159265f
static float fsin(float x) {
    while (x >  PI) x -= 2*PI;
    while (x < -PI) x += 2*PI;
    float x2=x*x, x3=x*x2, x5=x3*x2, x7=x5*x2;
    return x - x3/6.0f + x5/120.0f - x7/5040.0f;
}
static float fcos(float x) { return fsin(x + PI/2); }
static float fsqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float r = x > 1.0f ? x * 0.5f : 1.0f;
    for (int i = 0; i < 12; i++) r = 0.5f * (r + x / r);
    return r;
}

static unsigned *fb;       /* W*H 0x00RRGGBB framebuffer */
static float    *zbuf;     /* W*H depth (camera-space z; smaller = nearer) */

/* ---- mesh -----------------------------------------------------------------*/
#define MAXV 4096
#define MAXT 8192
static float vx[MAXV], vy[MAXV], vz[MAXV];          /* positions */
static float nx[MAXV], ny[MAXV], nz[MAXV];          /* vertex normals */
static int   tri[MAXT][3];
static int   nverts, ntris;
/* per-vertex base colour (0..1), set per model */
static float cr[MAXV], cg[MAXV], cb[MAXV];

static int addv(float x, float y, float z, float nxx, float nyy, float nzz) {
    int i = nverts++;
    vx[i]=x; vy[i]=y; vz[i]=z; nx[i]=nxx; ny[i]=nyy; nz[i]=nzz;
    return i;
}
static void addt(int a, int b, int c) { tri[ntris][0]=a; tri[ntris][1]=b; tri[ntris][2]=c; ntris++; }

static void set_palette(int model) {
    for (int i = 0; i < nverts; i++) {
        /* a smooth position-based colour so the smooth shading really shows */
        if (model == 0) { float ty=vy[i]*0.5f+0.5f; cr[i]=0.13f+0.30f*ty; cg[i]=0.45f+0.35f*ty; cb[i]=0.95f; }   /* sphere: vivid cyan-blue */
        else if (model == 1) { cr[i]=1.0f; cg[i]=0.45f+0.45f*(vx[i]*0.4f+0.5f); cb[i]=0.25f; }                       /* torus: orange/gold */
        else { cr[i]=0.5f+0.5f*(vx[i]*0.5f+0.5f); cg[i]=0.5f+0.5f*(vy[i]*0.5f+0.5f); cb[i]=0.5f+0.5f*(vz[i]*0.5f+0.5f); } /* cube: rgb */
    }
}

static void gen_sphere(void) {
    nverts = ntris = 0;
    int LA = 22, LO = 34;                 /* latitude / longitude bands */
    for (int a = 0; a <= LA; a++) {
        float th = PI * a / LA;           /* 0..pi */
        float st = fsin(th), ct = fcos(th);
        for (int o = 0; o <= LO; o++) {
            float ph = 2*PI * o / LO;
            float x = st*fcos(ph), y = ct, z = st*fsin(ph);
            addv(x, y, z, x, y, z);       /* unit sphere: normal == position */
        }
    }
    int stride = LO + 1;
    for (int a = 0; a < LA; a++)
        for (int o = 0; o < LO; o++) {
            int p0 = a*stride+o, p1 = p0+1, p2 = p0+stride, p3 = p2+1;
            addt(p0, p2, p1); addt(p1, p2, p3);
        }
}
static void gen_torus(void) {
    nverts = ntris = 0;
    int NU = 40, NV = 20; float R = 0.72f, r = 0.32f;
    for (int u = 0; u <= NU; u++) {
        float au = 2*PI*u/NU, cu=fcos(au), su=fsin(au);
        for (int v = 0; v <= NV; v++) {
            float av = 2*PI*v/NV, cv=fcos(av), sv=fsin(av);
            float x=(R+r*cv)*cu, y=r*sv, z=(R+r*cv)*su;
            addv(x, y, z, cv*cu, sv, cv*su);
        }
    }
    int stride = NV + 1;
    for (int u = 0; u < NU; u++)
        for (int v = 0; v < NV; v++) {
            int p0=u*stride+v, p1=p0+1, p2=p0+stride, p3=p2+1;
            addt(p0, p2, p1); addt(p1, p2, p3);
        }
}
static void gen_cube(void) {
    nverts = ntris = 0;
    /* per-face vertices (flat normals) so the cube stays crisp under Gouraud */
    static const float fn[6][3]={{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    static const float fv[6][4][3]={
        {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}, {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}},
        {{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1}}, {{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}},
        {{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}}, {{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}},
    };
    for (int f = 0; f < 6; f++) {
        int base = nverts;
        for (int k = 0; k < 4; k++)
            addv(fv[f][k][0]*0.8f, fv[f][k][1]*0.8f, fv[f][k][2]*0.8f, fn[f][0], fn[f][1], fn[f][2]);
        addt(base, base+1, base+2); addt(base, base+2, base+3);
    }
}

/* ---- Gouraud + Z-buffer triangle rasterizer ------------------------------ */
typedef struct { float x, y, z, r, g, b; } SV;   /* screen vertex + attributes */

static void edge(const SV *a, const SV *b, float y, float *x, float *z, float *r, float *g, float *bl) {
    float t = (b->y == a->y) ? 0.0f : (y - a->y) / (b->y - a->y);
    *x = a->x + (b->x - a->x)*t;  *z = a->z + (b->z - a->z)*t;
    *r = a->r + (b->r - a->r)*t;  *g = a->g + (b->g - a->g)*t;  *bl = a->b + (b->b - a->b)*t;
}
static void raster(SV a, SV b, SV c) {
    /* sort by y: a top, c bottom */
    SV t;
    if (b.y < a.y) { t=a; a=b; b=t; }
    if (c.y < a.y) { t=a; a=c; c=t; }
    if (c.y < b.y) { t=b; b=c; c=t; }
    int y0 = (int)(a.y + 0.5f), y2 = (int)(c.y + 0.5f);
    if (y2 <= y0) return;
    if (y0 < 0) y0 = 0; if (y2 > H) y2 = H;
    for (int y = y0; y < y2; y++) {
        float fy = y + 0.5f;
        float xl,zl,rl,gl,bll, xr,zr,rr,gr,brr, xb,zb,rb,gb,bb;
        edge(&a, &c, fy, &xl, &zl, &rl, &gl, &bll);      /* long edge */
        if (fy < b.y) edge(&a, &b, fy, &xr, &zr, &rr, &gr, &brr);   /* upper */
        else          edge(&b, &c, fy, &xr, &zr, &rr, &gr, &brr);   /* lower */
        if (xr < xl) {  /* make left<right, swapping all attrs */
            float s;
            s=xl;xl=xr;xr=s; s=zl;zl=zr;zr=s; s=rl;rl=rr;rr=s; s=gl;gl=gr;gr=s; s=bll;bll=brr;brr=s;
        }
        int x0 = (int)(xl + 0.5f), x1 = (int)(xr + 0.5f);
        if (x0 < 0) x0 = 0; if (x1 > W) x1 = W;
        float span = (xr - xl); if (span < 0.001f) span = 0.001f;
        unsigned *row = fb + (long)y * W;
        float    *zr2 = zbuf + (long)y * W;
        for (int x = x0; x < x1; x++) {
            float tx = (x + 0.5f - xl) / span;
            float z = zl + (zr - zl)*tx;
            if (z >= zr2[x]) continue;               /* z-test (nearer wins) */
            zr2[x] = z;
            float R = rl + (rr - rl)*tx, G = gl + (gr - gl)*tx, B = bll + (brr - bll)*tx;
            int ri = (int)(R*255.0f), gi = (int)(G*255.0f), bi = (int)(B*255.0f);
            if (ri<0)ri=0; if(ri>255)ri=255; if(gi<0)gi=0; if(gi>255)gi=255; if(bi<0)bi=0; if(bi>255)bi=255;
            row[x] = ((unsigned)ri<<16)|((unsigned)gi<<8)|(unsigned)bi;
        }
    }
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("scene3d: graphics init failed\n"); return 1; }
    fb   = malloc((unsigned long)W*H*4);
    zbuf = malloc((unsigned long)W*H*sizeof(float));
    if (!fb || !zbuf) { print("scene3d: out of memory\n"); return 1; }
    sys_caret(0);

    int model = 0;
    gen_sphere(); set_palette(model);

    float spin = 0.0f, autospin = 1.0f;
    float camYaw = 0.4f, camPitch = 0.35f, dist = 3.4f;

    /* light directions (will be normalized) */
    float lx = 0.4f, ly = 0.7f, lz = 0.6f;
    float ll = fsqrt(lx*lx+ly*ly+lz*lz); lx/=ll; ly/=ll; lz/=ll;
    float fxl = -0.5f, fyl = 0.2f, fzl = -0.5f;           /* rim/fill light */
    float fl = fsqrt(fxl*fxl+fyl*fyl+fzl*fzl); fxl/=fl; fyl/=fl; fzl/=fl;

    for (;;) {
        int k = sys_pollkey();
        if (k=='q'||k=='Q'||k==27) break;
        if (k==' ') { model=(model+1)%3; if(model==0)gen_sphere(); else if(model==1)gen_torus(); else gen_cube(); set_palette(model); }
        if (k=='+'||k=='=') { dist -= 0.3f; if (dist<1.8f) dist=1.8f; }
        if (k=='-'||k=='_') { dist += 0.3f; if (dist>8.0f) dist=8.0f; }
        if (k=='r'||k=='R') autospin = autospin>0.5f ? 0.0f : 1.0f;
        /* arrow keys orbit the camera (raw scancodes: L=0x4B R=0x4D U=0x48 D=0x50) */
        int e;
        while ((e = sys_getkbevent()) >= 0) {
            int sc = e & 0xFF;
            if (e & 0x200) {  /* extended (arrows) */
                if (sc==0x4B) camYaw   -= 0.12f;
                if (sc==0x4D) camYaw   += 0.12f;
                if (sc==0x48) camPitch -= 0.10f;
                if (sc==0x50) camPitch += 0.10f;
            }
        }
        if (camPitch >  1.4f) camPitch =  1.4f;
        if (camPitch < -1.4f) camPitch = -1.4f;

        spin += 0.02f * autospin;

        /* gradient background + clear z-buffer */
        for (int y = 0; y < H; y++) {
            int sh = 18 + y*40/H;
            unsigned bg = ((unsigned)(sh/2)<<16)|((unsigned)(sh/2)<<8)|(unsigned)(sh+12);
            unsigned *row = fb + (long)y*W; float *zr = zbuf + (long)y*W;
            for (int x = 0; x < W; x++) { row[x]=bg; zr[x]=1e30f; }
        }

        /* combined model-spin + camera-orbit rotation (Y by spin+yaw, then X by pitch) */
        float ay = spin + camYaw, ax = camPitch;
        float cy=fcos(ay), sy=fsin(ay), cx=fcos(ax), sx=fsin(ax);
        float focal = 1.3f * (H/2);

        static SV sv[MAXV];
        for (int i = 0; i < nverts; i++) {
            /* rotate position */
            float x1 = vx[i]*cy - vz[i]*sy, z1 = vx[i]*sy + vz[i]*cy, y1 = vy[i];
            float y2 = y1*cx - z1*sx, z2 = y1*sx + z1*cx;
            float zc = z2 + dist;                       /* camera space */
            /* rotate normal the same way */
            float Nx1 = nx[i]*cy - nz[i]*sy, Nz1 = nx[i]*sy + nz[i]*cy, Ny1 = ny[i];
            float Ny2 = Ny1*cx - Nz1*sx, Nz2 = Ny1*sx + Nz1*cx;
            /* lighting: ambient + diffuse(key) + diffuse(fill) */
            float d1 = Nx1*lx + Ny2*ly + Nz2*lz; if (d1<0) d1=0;
            float d2 = Nx1*fxl + Ny2*fyl + Nz2*fzl; if (d2<0) d2=0;
            float lit = 0.18f + 0.70f*d1 + 0.16f*d2;
            /* a touch of specular for sheen (kept small so material colour shows) */
            float spec = d1*d1; spec*=spec; spec*=spec; spec*=0.45f;   /* 0.45*d1^8 */
            sv[i].x = W/2 + (x1*focal)/zc;
            sv[i].y = H/2 + (y2*focal)/zc;
            sv[i].z = zc;
            sv[i].r = cr[i]*lit + spec;
            sv[i].g = cg[i]*lit + spec;
            sv[i].b = cb[i]*lit + spec;
        }

        for (int t = 0; t < ntris; t++) {
            SV *A=&sv[tri[t][0]], *B=&sv[tri[t][1]], *C=&sv[tri[t][2]];
            /* backface cull in screen space (CCW front) */
            float cross = (B->x-A->x)*(C->y-A->y) - (B->y-A->y)*(C->x-A->x);
            if (cross <= 0) continue;
            raster(*A, *B, *C);
        }

        sys_gfx_blit(fb);
        sys_sleep(16);
    }
    free(fb); free(zbuf);
    return 0;
}
