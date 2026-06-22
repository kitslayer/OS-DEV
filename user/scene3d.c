/*
 * scene3d.c — a real-time software 3D engine for OS-DEV. No GPU.
 *
 * A from-scratch perspective 3D renderer: procedurally-generated triangle meshes
 * (UV sphere, torus, cube), transformed by a rotation/camera matrix, lit by a
 * directional light (+ ambient + a rim fill light), and rasterized with a
 * per-pixel Z-BUFFER and GOURAUD shading. Each vertex also carries TEXTURE
 * coordinates, and a procedural texture (a coloured checker + grid, per model) is
 * mapped onto the surface and modulated by the lighting — so you get a lit,
 * textured solid. Built with SSE so the geometry/lighting math is plain float
 * (the generic user rule is -mgeneral-regs-only); trig/sqrt are tiny
 * self-contained approximations (no libm), exactly like the raycaster.
 *
 * Controls: SPACE = next model · arrows = orbit the camera · +/- = zoom ·
 * T = texture on/off · R = toggle auto-spin · Q/Esc = quit.
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

/* ---- procedural texture (regenerated per model) -------------------------- */
#define TW 256
#define TH 256
static unsigned *tex;      /* TW*TH texture */
static int textured = 1;

static void gen_texture(int model) {
    for (int v = 0; v < TH; v++)
        for (int u = 0; u < TW; u++) {
            int checker = (((u >> 5) ^ (v >> 5)) & 1);
            int grid    = ((u & 31) < 2 || (v & 31) < 2);
            int r, g, b;
            if (model == 0) {                 /* sphere: blue / cyan tech grid */
                r = checker ? 30 : 12;  g = checker ? 110 : 60;  b = checker ? 230 : 150;
                if (grid) { r = 120; g = 230; b = 255; }
            } else if (model == 1) {          /* torus: gold / amber */
                r = checker ? 220 : 150;  g = checker ? 150 : 95;  b = checker ? 40 : 20;
                if (grid) { r = 255; g = 225; b = 130; }
            } else {                          /* cube: red / blue crate */
                r = checker ? 200 : 60;  g = 60;  b = checker ? 60 : 200;
                if (grid) { r = 240; g = 240; b = 240; }
            }
            tex[v * TW + u] = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
        }
}

/* ---- mesh -----------------------------------------------------------------*/
#define MAXV 4096
#define MAXT 8192
static float vx[MAXV], vy[MAXV], vz[MAXV];          /* positions */
static float nx[MAXV], ny[MAXV], nz[MAXV];          /* vertex normals */
static float tu[MAXV], tv[MAXV];                    /* texture coords */
static int   tri[MAXT][3];
static int   nverts, ntris;

static int addv(float x, float y, float z, float nxx, float nyy, float nzz, float u, float vv) {
    int i = nverts++;
    vx[i]=x; vy[i]=y; vz[i]=z; nx[i]=nxx; ny[i]=nyy; nz[i]=nzz; tu[i]=u; tv[i]=vv;
    return i;
}
static void addt(int a, int b, int c) { tri[ntris][0]=a; tri[ntris][1]=b; tri[ntris][2]=c; ntris++; }

static void gen_sphere(void) {
    nverts = ntris = 0;
    int LA = 22, LO = 34;
    for (int a = 0; a <= LA; a++) {
        float th = PI * a / LA;
        float st = fsin(th), ct = fcos(th);
        for (int o = 0; o <= LO; o++) {
            float ph = 2*PI * o / LO;
            float x = st*fcos(ph), y = ct, z = st*fsin(ph);
            addv(x, y, z, x, y, z, (float)o / LO * 4.0f, (float)a / LA * 2.0f);
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
            addv(x, y, z, cv*cu, sv, cv*su, (float)u / NU * 6.0f, (float)v / NV * 1.0f);
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
    static const float fn[6][3]={{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    static const float fv[6][4][3]={
        {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}, {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}},
        {{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1}}, {{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}},
        {{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}}, {{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}},
    };
    static const float fuv[4][2]={{0,0},{1,0},{1,1},{0,1}};
    for (int f = 0; f < 6; f++) {
        int base = nverts;
        for (int k = 0; k < 4; k++)
            addv(fv[f][k][0]*0.8f, fv[f][k][1]*0.8f, fv[f][k][2]*0.8f,
                 fn[f][0], fn[f][1], fn[f][2], fuv[k][0], fuv[k][1]);
        addt(base, base+1, base+2); addt(base, base+2, base+3);
    }
}

/* ---- Gouraud + texture + Z-buffer triangle rasterizer -------------------- */
/* Screen vertex: position, depth, texcoord, light intensity. (Affine interp —
 * the meshes are densely tessellated, so it reads clean.) */
typedef struct { float x, y, z, u, v, l; } SV;

static void edge(const SV *a, const SV *b, float y,
                 float *x, float *z, float *u, float *v, float *l) {
    float t = (b->y == a->y) ? 0.0f : (y - a->y) / (b->y - a->y);
    *x = a->x + (b->x - a->x)*t;  *z = a->z + (b->z - a->z)*t;
    *u = a->u + (b->u - a->u)*t;  *v = a->v + (b->v - a->v)*t;  *l = a->l + (b->l - a->l)*t;
}
static unsigned sample(float u, float v) {
    int U = (int)(u * TW), V = (int)(v * TH);
    U &= (TW - 1); V &= (TH - 1);    /* wrap (TW/TH are powers of two) */
    if (U < 0) U += TW; if (V < 0) V += TH;
    return tex[V * TW + U];
}
static void raster(SV a, SV b, SV c) {
    SV t;
    if (b.y < a.y) { t=a; a=b; b=t; }
    if (c.y < a.y) { t=a; a=c; c=t; }
    if (c.y < b.y) { t=b; b=c; c=t; }
    int y0 = (int)(a.y + 0.5f), y2 = (int)(c.y + 0.5f);
    if (y2 <= y0) return;
    if (y0 < 0) y0 = 0; if (y2 > H) y2 = H;
    for (int y = y0; y < y2; y++) {
        float fy = y + 0.5f;
        float xl,zl,ul,vl,ll, xr,zr,ur,vr,lr;
        edge(&a, &c, fy, &xl, &zl, &ul, &vl, &ll);
        if (fy < b.y) edge(&a, &b, fy, &xr, &zr, &ur, &vr, &lr);
        else          edge(&b, &c, fy, &xr, &zr, &ur, &vr, &lr);
        if (xr < xl) {
            float s;
            s=xl;xl=xr;xr=s; s=zl;zl=zr;zr=s; s=ul;ul=ur;ur=s; s=vl;vl=vr;vr=s; s=ll;ll=lr;lr=s;
        }
        int x0 = (int)(xl + 0.5f), x1 = (int)(xr + 0.5f);
        if (x0 < 0) x0 = 0; if (x1 > W) x1 = W;
        float span = (xr - xl); if (span < 0.001f) span = 0.001f;
        unsigned *row = fb + (long)y * W;
        float    *zr2 = zbuf + (long)y * W;
        for (int x = x0; x < x1; x++) {
            float tx = (x + 0.5f - xl) / span;
            float z = zl + (zr - zl)*tx;
            if (z >= zr2[x]) continue;
            zr2[x] = z;
            float li = ll + (lr - ll)*tx;
            unsigned tc = sample(ul + (ur - ul)*tx, vl + (vr - vl)*tx);
            int R = (int)(((tc >> 16) & 255) * li);
            int G = (int)(((tc >> 8)  & 255) * li);
            int B = (int)(((tc)       & 255) * li);
            if (R>255)R=255; if(G>255)G=255; if(B>255)B=255;
            row[x] = ((unsigned)R<<16)|((unsigned)G<<8)|(unsigned)B;
        }
    }
}

int main(void) {
    if (sys_gfx_init(W, H) < 0) { print("scene3d: graphics init failed\n"); return 1; }
    fb   = malloc((unsigned long)W*H*4);
    zbuf = malloc((unsigned long)W*H*sizeof(float));
    tex  = malloc((unsigned long)TW*TH*4);
    if (!fb || !zbuf || !tex) { print("scene3d: out of memory\n"); return 1; }
    sys_caret(0);

    int model = 0;
    gen_sphere(); gen_texture(model);

    float spin = 0.0f, autospin = 1.0f;
    float camYaw = 0.4f, camPitch = 0.35f, dist = 3.4f;

    float lx = 0.4f, ly = 0.7f, lz = 0.6f;
    float ll = fsqrt(lx*lx+ly*ly+lz*lz); lx/=ll; ly/=ll; lz/=ll;
    float fxl = -0.5f, fyl = 0.2f, fzl = -0.5f;
    float fl = fsqrt(fxl*fxl+fyl*fyl+fzl*fzl); fxl/=fl; fyl/=fl; fzl/=fl;

    for (;;) {
        int k = sys_pollkey();
        if (k=='q'||k=='Q'||k==27) break;
        if (k==' ') { model=(model+1)%3; if(model==0)gen_sphere(); else if(model==1)gen_torus(); else gen_cube(); gen_texture(model); }
        if (k=='+'||k=='=') { dist -= 0.3f; if (dist<1.8f) dist=1.8f; }
        if (k=='-'||k=='_') { dist += 0.3f; if (dist>8.0f) dist=8.0f; }
        if (k=='r'||k=='R') autospin = autospin>0.5f ? 0.0f : 1.0f;
        if (k=='t'||k=='T') textured = !textured;
        int e;
        while ((e = sys_getkbevent()) >= 0) {
            int sc = e & 0xFF;
            if (e & 0x200) {
                if (sc==0x4B) camYaw   -= 0.12f;
                if (sc==0x4D) camYaw   += 0.12f;
                if (sc==0x48) camPitch -= 0.10f;
                if (sc==0x50) camPitch += 0.10f;
            }
        }
        if (camPitch >  1.4f) camPitch =  1.4f;
        if (camPitch < -1.4f) camPitch = -1.4f;

        spin += 0.02f * autospin;

        for (int y = 0; y < H; y++) {
            int sh = 18 + y*40/H;
            unsigned bg = ((unsigned)(sh/2)<<16)|((unsigned)(sh/2)<<8)|(unsigned)(sh+12);
            unsigned *row = fb + (long)y*W; float *zr = zbuf + (long)y*W;
            for (int x = 0; x < W; x++) { row[x]=bg; zr[x]=1e30f; }
        }

        float ay = spin + camYaw, ax = camPitch;
        float cy=fcos(ay), sy=fsin(ay), cx=fcos(ax), sx=fsin(ax);
        float focal = 1.3f * (H * 0.5f);

        static SV sv[MAXV];
        for (int i = 0; i < nverts; i++) {
            float x1 = vx[i]*cy - vz[i]*sy, z1 = vx[i]*sy + vz[i]*cy, y1 = vy[i];
            float y2 = y1*cx - z1*sx, z2 = y1*sx + z1*cx;
            float zc = z2 + dist;
            float Nx1 = nx[i]*cy - nz[i]*sy, Nz1 = nx[i]*sy + nz[i]*cy, Ny1 = ny[i];
            float Ny2 = Ny1*cx - Nz1*sx, Nz2 = Ny1*sx + Nz1*cx;
            float d1 = Nx1*lx + Ny2*ly + Nz2*lz; if (d1<0) d1=0;
            float d2 = Nx1*fxl + Ny2*fyl + Nz2*fzl; if (d2<0) d2=0;
            float lit = 0.22f + 0.78f*d1 + 0.18f*d2;
            float spec = d1*d1; spec*=spec; spec*=spec; spec*=0.4f;
            sv[i].x = W*0.5f + (x1*focal)/zc;
            sv[i].y = H*0.5f + (y2*focal)/zc;
            sv[i].z = zc;
            sv[i].u = tu[i]; sv[i].v = tv[i];
            sv[i].l = lit + spec;
        }

        for (int t = 0; t < ntris; t++) {
            SV A=sv[tri[t][0]], B=sv[tri[t][1]], C=sv[tri[t][2]];
            float cross = (B.x-A.x)*(C.y-A.y) - (B.y-A.y)*(C.x-A.x);
            if (cross <= 0) continue;
            if (!textured) {                    /* untextured: force a neutral grey via uv->fixed texel */
                A.u=A.v=B.u=B.v=C.u=C.v=0.5f;   /* sample one texel; light still shades it */
            }
            raster(A, B, C);
        }

        sys_gfx_blit(fb);
        sys_sleep(16);
    }
    free(fb); free(zbuf); free(tex);
    return 0;
}
