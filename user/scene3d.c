/*
 * scene3d.c — a real-time software 3D engine for OS-DEV. No GPU.
 *
 * A from-scratch perspective 3D renderer: a procedurally-generated, spinning,
 * lit + TEXTURED model (UV sphere / torus / cube) standing on a checkered FLOOR,
 * with an orbiting camera. Rasterized with a per-pixel Z-BUFFER and
 * PERSPECTIVE-CORRECT texture + Gouraud-light interpolation (interpolate 1/z,
 * u/z, v/z, light/z; divide per pixel), plus backface culling and near-plane
 * culling. Built with SSE so the math is plain float (the generic user rule is
 * -mgeneral-regs-only); trig/sqrt are tiny self-contained approximations (no
 * libm), like the raycaster.
 *
 * Controls: SPACE = next model · arrows = orbit the camera · +/- = zoom ·
 * T = texture on/off · F = floor on/off · R = auto-spin · Q/Esc = quit.
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
static float    *zbuf;     /* W*H depth as 1/z (LARGER = nearer) */

/* ---- procedural textures (regenerated per model) ------------------------- */
#define TW 256
#define TH 256
static unsigned *tex;      /* model texture */
static unsigned *ftex;     /* floor texture (checker) */
static unsigned *curtex;   /* the texture the rasterizer currently samples */
static int textured = 1, show_floor = 1;
static unsigned flatcol = 0x8090a0;   /* model colour when texture is off */

static void gen_texture(int model) {
    int br=0,bg=0,bb=0;
    for (int v = 0; v < TH; v++)
        for (int u = 0; u < TW; u++) {
            int checker = (((u >> 5) ^ (v >> 5)) & 1);
            int grid    = ((u & 31) < 2 || (v & 31) < 2);
            int r, g, b;
            if (model == 0) { r = checker?70:45;  g = checker?150:110;  b = checker?240:200; if(grid){r=150;g=240;b=255;} }
            else if (model == 1) { r = checker?220:150; g = checker?150:95; b = checker?40:20; if(grid){r=255;g=225;b=130;} }
            else { r = checker?200:60; g=60; b = checker?60:200; if(grid){r=240;g=240;b=240;} }
            tex[v * TW + u] = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
            if (u==TW/2 && v==TH/2) { br=r; bg=g; bb=b; }
        }
    flatcol = ((unsigned)br<<16)|((unsigned)bg<<8)|(unsigned)bb;
}
static void gen_floor_texture(void) {
    for (int v = 0; v < TH; v++)
        for (int u = 0; u < TW; u++) {
            int checker = (((u >> 7) ^ (v >> 7)) & 1);   /* big 2x2 cells per tile */
            int s = checker ? 150 : 60;
            int grid = ((u & 127) < 3 || (v & 127) < 3);
            if (grid) s = 200;
            ftex[v*TW+u] = ((unsigned)s<<16)|((unsigned)s<<8)|(unsigned)(s+20);
        }
}

/* ---- mesh -----------------------------------------------------------------*/
#define MAXV 4096
#define MAXT 8192
static float vx[MAXV], vy[MAXV], vz[MAXV];
static float nx[MAXV], ny[MAXV], nz[MAXV];
static float tu[MAXV], tv[MAXV];
static int   tri[MAXT][3];
static int   nverts, ntris;

static int addv(float x, float y, float z, float a, float b, float c, float u, float w) {
    int i = nverts++;
    vx[i]=x; vy[i]=y; vz[i]=z; nx[i]=a; ny[i]=b; nz[i]=c; tu[i]=u; tv[i]=w;
    return i;
}
static void addt(int a, int b, int c) { tri[ntris][0]=a; tri[ntris][1]=b; tri[ntris][2]=c; ntris++; }

static void gen_sphere(void) {
    nverts = ntris = 0;
    int LA = 22, LO = 34;
    for (int a = 0; a <= LA; a++) {
        float th = PI * a / LA, st = fsin(th), ct = fcos(th);
        for (int o = 0; o <= LO; o++) {
            float ph = 2*PI * o / LO, x = st*fcos(ph), y = ct, z = st*fsin(ph);
            addv(x, y, z, x, y, z, (float)o/LO*4.0f, (float)a/LA*2.0f);
        }
    }
    int stride = LO + 1;
    for (int a = 0; a < LA; a++) for (int o = 0; o < LO; o++) {
        int p0=a*stride+o,p1=p0+1,p2=p0+stride,p3=p2+1; addt(p0,p2,p1); addt(p1,p2,p3);
    }
}
static void gen_torus(void) {
    nverts = ntris = 0;
    int NU = 40, NV = 20; float R = 0.72f, r = 0.32f;
    for (int u = 0; u <= NU; u++) {
        float au=2*PI*u/NU, cu=fcos(au), su=fsin(au);
        for (int v = 0; v <= NV; v++) {
            float av=2*PI*v/NV, cv=fcos(av), sv=fsin(av);
            addv((R+r*cv)*cu, r*sv, (R+r*cv)*su, cv*cu, sv, cv*su, (float)u/NU*6.0f, (float)v/NV*1.0f);
        }
    }
    int stride = NV + 1;
    for (int u = 0; u < NU; u++) for (int v = 0; v < NV; v++) {
        int p0=u*stride+v,p1=p0+1,p2=p0+stride,p3=p2+1; addt(p0,p2,p1); addt(p1,p2,p3);
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
            addv(fv[f][k][0]*0.8f, fv[f][k][1]*0.8f, fv[f][k][2]*0.8f, fn[f][0],fn[f][1],fn[f][2], fuv[k][0],fuv[k][1]);
        addt(base,base+1,base+2); addt(base,base+2,base+3);
    }
}

/* ---- camera / lighting (file-scope so the projector can read them) -------- */
static float lx,ly,lz, fxl,fyl,fzl, focal;
#define NEARZ 0.12f

/* A screen vertex: position + 1/z + perspective-correct (u/z, v/z, light/z). */
typedef struct { float x, y, iz, uz, vz, lz; } SV;

/* Transform one object-space vertex (+ its normal, uv) by yaw/pitch/dist, light
 * it, and project to screen with perspective-correct attributes. */
static SV project(float ox,float oy,float oz, float a,float b,float c,
                  float u,float w, float cy,float sy,float cx,float sx,float dist) {
    float x1=ox*cy - oz*sy, z1=ox*sy + oz*cy, y1=oy;
    float y2=y1*cx - z1*sx, z2=y1*sx + z1*cx;
    float zc=z2+dist; if (zc < NEARZ) zc = NEARZ;
    float Nx1=a*cy - c*sy, Nz1=a*sy + c*cy, Ny1=b;
    float Ny2=Ny1*cx - Nz1*sx, Nz2=Ny1*sx + Nz1*cx;
    float d1=Nx1*lx + Ny2*ly + Nz2*lz; if (d1<0) d1=0;
    float d2=Nx1*fxl + Ny2*fyl + Nz2*fzl; if (d2<0) d2=0;
    float spec=d1*d1; spec*=spec; spec*=spec; spec*=0.4f;
    float lit=0.22f + 0.78f*d1 + 0.18f*d2 + spec;
    float iz=1.0f/zc;
    SV s;
    s.x = W*0.5f + (x1*focal)*iz;
    s.y = H*0.5f + (y2*focal)*iz;
    s.iz = iz; s.uz = u*iz; s.vz = w*iz; s.lz = lit*iz;
    return s;
}

static unsigned sample(float u, float v) {
    int U=(int)(u*TW), V=(int)(v*TH);
    U &= (TW-1); V &= (TH-1);
    return curtex[V*TW+U];
}
static void edge(const SV*a,const SV*b,float y,float*x,float*iz,float*uz,float*vz,float*lz){
    float t=(b->y==a->y)?0.0f:(y-a->y)/(b->y-a->y);
    *x=a->x+(b->x-a->x)*t; *iz=a->iz+(b->iz-a->iz)*t;
    *uz=a->uz+(b->uz-a->uz)*t; *vz=a->vz+(b->vz-a->vz)*t; *lz=a->lz+(b->lz-a->lz)*t;
}
static void raster(SV a, SV b, SV c) {
    SV t;
    if (b.y<a.y){t=a;a=b;b=t;} if (c.y<a.y){t=a;a=c;c=t;} if (c.y<b.y){t=b;b=c;c=t;}
    int y0=(int)(a.y+0.5f), y2=(int)(c.y+0.5f);
    if (y2<=y0) return;
    if (y0<0) y0=0; if (y2>H) y2=H;
    for (int y=y0; y<y2; y++) {
        float fy=y+0.5f, xl,izl,uzl,vzl,lzl, xr,izr,uzr,vzr,lzr;
        edge(&a,&c,fy,&xl,&izl,&uzl,&vzl,&lzl);
        if (fy<b.y) edge(&a,&b,fy,&xr,&izr,&uzr,&vzr,&lzr);
        else        edge(&b,&c,fy,&xr,&izr,&uzr,&vzr,&lzr);
        if (xr<xl){ float s; s=xl;xl=xr;xr=s; s=izl;izl=izr;izr=s; s=uzl;uzl=uzr;uzr=s; s=vzl;vzl=vzr;vzr=s; s=lzl;lzl=lzr;lzr=s; }
        int x0=(int)(xl+0.5f), x1=(int)(xr+0.5f);
        if (x0<0) x0=0; if (x1>W) x1=W;
        float span=(xr-xl); if (span<0.001f) span=0.001f;
        unsigned *row=fb+(long)y*W; float *zr=zbuf+(long)y*W;
        for (int x=x0; x<x1; x++) {
            float tx=(x+0.5f-xl)/span;
            float iz=izl+(izr-izl)*tx;
            if (iz<=zr[x]) continue;           /* nearer = larger 1/z */
            zr[x]=iz;
            float w=1.0f/iz;
            float li=(lzl+(lzr-lzl)*tx)*w;
            unsigned tc = textured ? sample((uzl+(uzr-uzl)*tx)*w, (vzl+(vzr-vzl)*tx)*w) : flatcol;
            int R=(int)(((tc>>16)&255)*li), G=(int)(((tc>>8)&255)*li), B=(int)((tc&255)*li);
            if(R>255)R=255; if(G>255)G=255; if(B>255)B=255;
            row[x]=((unsigned)R<<16)|((unsigned)G<<8)|(unsigned)B;
        }
    }
}

int main(void) {
    if (sys_gfx_init(W,H) < 0) { print("scene3d: graphics init failed\n"); return 1; }
    fb   = malloc((unsigned long)W*H*4);
    zbuf = malloc((unsigned long)W*H*sizeof(float));
    tex  = malloc((unsigned long)TW*TH*4);
    ftex = malloc((unsigned long)TW*TH*4);
    if (!fb||!zbuf||!tex||!ftex) { print("scene3d: out of memory\n"); return 1; }
    sys_caret(0);
    gen_floor_texture();

    int model = 0;
    gen_sphere(); gen_texture(model);

    float spin=0.0f, autospin=1.0f;
    float camYaw=0.5f, camPitch=0.45f, dist=4.2f;

    lx=0.4f; ly=0.7f; lz=0.6f;
    float l=fsqrt(lx*lx+ly*ly+lz*lz); lx/=l; ly/=l; lz/=l;
    fxl=-0.5f; fyl=0.2f; fzl=-0.5f;
    float fl=fsqrt(fxl*fxl+fyl*fyl+fzl*fzl); fxl/=fl; fyl/=fl; fzl/=fl;
    focal = 1.3f * (H * 0.5f);          /* perspective focal length (was missing!) */

    static SV sv[MAXV];

    for (;;) {
        int k = sys_pollkey();
        if (k=='q'||k=='Q'||k==27) break;
        if (k==' ') { model=(model+1)%3; if(model==0)gen_sphere(); else if(model==1)gen_torus(); else gen_cube(); gen_texture(model); }
        if (k=='+'||k=='=') { dist-=0.3f; if(dist<2.2f)dist=2.2f; }
        if (k=='-'||k=='_') { dist+=0.3f; if(dist>9.0f)dist=9.0f; }
        if (k=='r'||k=='R') autospin = autospin>0.5f?0.0f:1.0f;
        if (k=='t'||k=='T') textured = !textured;
        if (k=='f'||k=='F') show_floor = !show_floor;
        int e;
        while ((e=sys_getkbevent())>=0) {
            int sc=e&0xFF;
            if (e&0x200) { if(sc==0x4B)camYaw-=0.12f; if(sc==0x4D)camYaw+=0.12f; if(sc==0x48)camPitch-=0.10f; if(sc==0x50)camPitch+=0.10f; }
        }
        if (camPitch>1.45f)camPitch=1.45f; if (camPitch<0.05f)camPitch=0.05f;
        spin += 0.02f*autospin;

        for (int y=0; y<H; y++) {                 /* gradient sky + clear z (1/z=0 == farthest) */
            int sh=20 + y*55/H;
            unsigned bg=((unsigned)(sh/3)<<16)|((unsigned)(sh/2)<<8)|(unsigned)(sh+25);
            unsigned *row=fb+(long)y*W; float *zr=zbuf+(long)y*W;
            for (int x=0; x<W; x++){ row[x]=bg; zr[x]=0.0f; }
        }

        float cxp=fcos(camPitch), sxp=fsin(camPitch);

        /* --- floor: a grid of tiles, world-static (camera yaw only), near-culled --- */
        if (show_floor) {
            curtex = ftex;
            float cyf=fcos(camYaw), syf=fsin(camYaw);
            int FN=18; float FE=9.0f, FY=-1.25f, step=2*FE/FN;
            int saved_t = textured; textured = 1;     /* floor always textured */
            for (int gz=0; gz<FN; gz++) for (int gx=0; gx<FN; gx++) {
                float x0=-FE+gx*step, x1c=x0+step, z0=-FE+gz*step, z1c=z0+step;
                float u0=(float)gx, u1=(float)(gx+1), v0=(float)gz, v1=(float)(gz+1);
                SV q0=project(x0,FY,z0, 0,1,0, u0,v0, cyf,syf,cxp,sxp,dist);
                SV q1=project(x1c,FY,z0,0,1,0, u1,v0, cyf,syf,cxp,sxp,dist);
                SV q2=project(x1c,FY,z1c,0,1,0,u1,v1, cyf,syf,cxp,sxp,dist);
                SV q3=project(x0,FY,z1c,0,1,0, u0,v1, cyf,syf,cxp,sxp,dist);
                /* near-cull: skip a tile if any corner is nearer than z=0.5, so no
                 * near-plane-clamped (distorted) tile is ever drawn (clamp is at
                 * NEARZ=0.12, well inside 0.5) — otherwise such tiles smear bogus
                 * depth over the model. iz = 1/z, so z<0.5 <=> iz>2. */
                if (q0.iz>2.0f||q1.iz>2.0f||q2.iz>2.0f||q3.iz>2.0f) continue;
                raster(q0,q2,q1); raster(q0,q3,q2);
            }
            textured = saved_t;
        }

        /* --- the model: spin + camera orbit --- */
        curtex = tex;
        float cym=fcos(camYaw+spin), sym=fsin(camYaw+spin);
        for (int i=0; i<nverts; i++)
            sv[i]=project(vx[i],vy[i],vz[i], nx[i],ny[i],nz[i], tu[i],tv[i], cym,sym,cxp,sxp,dist);
        for (int t=0; t<ntris; t++) {
            SV A=sv[tri[t][0]], B=sv[tri[t][1]], C=sv[tri[t][2]];
            float cross=(B.x-A.x)*(C.y-A.y)-(B.y-A.y)*(C.x-A.x);
            if (cross<=0) continue;
            raster(A,B,C);
        }

        sys_gfx_blit(fb);
        sys_sleep(16);
    }
    free(fb); free(zbuf); free(tex); free(ftex);
    return 0;
}
