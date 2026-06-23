/*
 * scene3d.c — a real-time software 3D engine for OS-DEV. No GPU.
 *
 * A from-scratch perspective 3D renderer: a procedurally-generated, spinning,
 * lit + TEXTURED centerpiece (UV sphere / torus / cube) ringed by a little
 * ORRERY of tinted moons on tilted orbits, on a checkered FLOOR, with an
 * orbiting camera. Rasterized with a per-pixel Z-BUFFER and
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
static int textured = 1, show_floor = 1, shadowpass = 0;
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
            else if (model == 2) { r = checker?200:60; g=60; b = checker?60:200; if(grid){r=240;g=240;b=240;} }
            else { r = checker?40:30; g = checker?210:150; b = checker?185:140; if(grid){r=120;g=255;b=230;} }  /* knot: teal */
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

/* A dedicated sphere mesh for the orbiting satellites, snapshotted once at
 * startup — so the moons stay round even when the centerpiece morphs to a
 * torus or cube. */
static float mvx[MAXV],mvy[MAXV],mvz[MAXV], mnx[MAXV],mny[MAXV],mnz[MAXV], mtu[MAXV],mtv[MAXV];
static int   mtri[MAXT][3], mnv, mnt;

/* Per-object colour tint, 0..256 per channel (256 = identity / no tint). Set
 * per satellite so each moon has its own hue; reset to 256 for floor + model. */
static int tintR=256, tintG=256, tintB=256;

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
/* A (2,3) TORUS KNOT, rendered as a tube swept along the knot curve. The tube's
 * cross-section frame is parallel-transported (project the previous normal onto
 * the plane perpendicular to the new tangent) so it doesn't spin/kink along the
 * curve. Outward = the cross-section offset, which is exactly the surface normal,
 * so it lights + textures with the rest of the pipeline. */
static void gen_knot(void) {
    nverts = ntris = 0;
    int NU = 140, NV = 14; float tubeR = 0.42f, scale = 0.34f;
    float Nx = 0.0f, Ny = 1.0f, Nz = 0.0f;   /* transported normal (fixed up below) */
    for (int i = 0; i <= NU; i++) {
        float t = 2.0f*PI*i/NU, dt = 0.012f;
        float cx=(2.0f+fcos(3*t))*fcos(2*t), cy=(2.0f+fcos(3*t))*fsin(2*t), cz=fsin(3*t);
        float ax=(2.0f+fcos(3*(t+dt)))*fcos(2*(t+dt)), ay=(2.0f+fcos(3*(t+dt)))*fsin(2*(t+dt)), az=fsin(3*(t+dt));
        float tx=ax-cx, ty=ay-cy, tz=az-cz;                    /* tangent (finite difference) */
        float tl=fsqrt(tx*tx+ty*ty+tz*tz); if(tl<1e-5f)tl=1e-5f; tx/=tl; ty/=tl; tz/=tl;
        float d=Nx*tx+Ny*ty+Nz*tz; Nx-=d*tx; Ny-=d*ty; Nz-=d*tz;   /* parallel transport */
        float nl=fsqrt(Nx*Nx+Ny*Ny+Nz*Nz);
        if (nl<1e-4f) { Nx=1.0f; Ny=0.0f; Nz=0.0f; d=Nx*tx+Ny*ty+Nz*tz; Nx-=d*tx; Ny-=d*ty; Nz-=d*tz; nl=fsqrt(Nx*Nx+Ny*Ny+Nz*Nz); }
        Nx/=nl; Ny/=nl; Nz/=nl;
        float Bx=ty*Nz-tz*Ny, By=tz*Nx-tx*Nz, Bz=tx*Ny-ty*Nx;     /* binormal = T x N */
        for (int j = 0; j <= NV; j++) {
            float a=2.0f*PI*j/NV, ca=fcos(a), sa=fsin(a);
            float ox=Nx*ca+Bx*sa, oy=Ny*ca+By*sa, oz=Nz*ca+Bz*sa; /* outward (= surface normal) */
            addv((cx+tubeR*ox)*scale, (cy+tubeR*oy)*scale, (cz+tubeR*oz)*scale,
                 ox, oy, oz, (float)i/NU*9.0f, (float)j/NV*1.0f);
        }
    }
    int stride = NV+1;
    for (int i = 0; i < NU; i++) for (int j = 0; j < NV; j++) {
        int p0=i*stride+j, p1=p0+1, p2=p0+stride, p3=p2+1; addt(p0,p2,p1); addt(p1,p2,p3);
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
            if (shadowpass) {                  /* darken floor pixels under the cast shadow */
                if (zr[x] > 1e-4f) { unsigned p=row[x]; row[x]=(p>>1)&0x7f7f7fu; }
                continue;
            }
            float tx=(x+0.5f-xl)/span;
            float iz=izl+(izr-izl)*tx;
            if (iz<=zr[x]) continue;           /* nearer = larger 1/z */
            zr[x]=iz;
            float w=1.0f/iz;
            float li=(lzl+(lzr-lzl)*tx)*w;
            unsigned tc = textured ? sample((uzl+(uzr-uzl)*tx)*w, (vzl+(vzr-vzl)*tx)*w) : flatcol;
            int R=(int)(((tc>>16)&255)*li), G=(int)(((tc>>8)&255)*li), B=(int)((tc&255)*li);
            R=(R*tintR)>>8; G=(G*tintG)>>8; B=(B*tintB)>>8;   /* per-object tint (256 = identity) */
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
    /* snapshot the sphere as the satellite mesh (centerpiece can morph; moons stay round) */
    for (int i=0;i<nverts;i++){ mvx[i]=vx[i];mvy[i]=vy[i];mvz[i]=vz[i]; mnx[i]=nx[i];mny[i]=ny[i];mnz[i]=nz[i]; mtu[i]=tu[i];mtv[i]=tv[i]; }
    for (int t=0;t<ntris;t++){ mtri[t][0]=tri[t][0];mtri[t][1]=tri[t][1];mtri[t][2]=tri[t][2]; }
    mnv=nverts; mnt=ntris;

    float spin=0.0f, autospin=1.0f;
    float camYaw=0.5f, camPitch=0.45f, dist=4.2f;

    lx=0.4f; ly=0.7f; lz=0.6f;
    float l=fsqrt(lx*lx+ly*ly+lz*lz); lx/=l; ly/=l; lz/=l;
    fxl=-0.5f; fyl=0.2f; fzl=-0.5f;
    float fl=fsqrt(fxl*fxl+fyl*fyl+fzl*fzl); fxl/=fl; fyl/=fl; fzl/=fl;
    focal = 1.3f * (H * 0.5f);          /* perspective focal length (was missing!) */

    static SV sv[MAXV], sv2[MAXV];

    for (;;) {
        int k = sys_pollkey();
        if (k=='q'||k=='Q'||k==27) break;
        if (k==' ') { model=(model+1)%4; if(model==0)gen_sphere(); else if(model==1)gen_torus(); else if(model==2)gen_cube(); else gen_knot(); gen_texture(model); }
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

        /* --- skybox: vertical gradient + a sun glow + faint stars --- (clears z: 1/z=0 = farthest) */
        for (int y=0; y<H; y++) {
            int gt = y*256/H;                            /* 0 (top) .. 255 (horizon) */
            int sr = 10 + gt*70/256, sg = 14 + gt*95/256, sb = 46 + gt*78/256;  /* indigo -> warm teal */
            int dys = y - H/5;
            unsigned *row=fb+(long)y*W; float *zr=zbuf+(long)y*W;
            for (int x=0; x<W; x++) {
                int r=sr, g=sg, b=sb;
                int dxs = x - (W*3/4), d2 = dxs*dxs + dys*dys;   /* sun glow, upper-right */
                if (d2 < 11000) { int gl=(11000-d2)*200/11000; r+=gl; g+=(gl*9)/10; b+=gl/2; }
                if (y < H/3) {                                   /* faint stars in the upper sky */
                    unsigned h=(unsigned)(x*1973 + y*9277); h^=h>>13; h*=0x5bd1e995u; h^=h>>15;
                    if ((h & 1023u) == 0) { int s=150+(int)((h>>10)&63); r=s; g=s; b=s; }
                }
                if (r>255)r=255; if(g>255)g=255; if(b>255)b=255;
                row[x]=((unsigned)r<<16)|((unsigned)g<<8)|(unsigned)b;
                zr[x]=0.0f;
            }
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

        /* --- shadow: project the model's silhouette onto the floor along the light --- */
        if (show_floor) {
            shadowpass = 1;
            float cs=fcos(spin), ss=fsin(spin), ccy=fcos(camYaw), scy=fsin(camYaw);
            float FY=-1.249f;
            for (int t=0; t<ntris; t++) {
                SV S[3];
                for (int j=0; j<3; j++) {
                    int vi=tri[t][j];
                    float wx=vx[vi]*cs - vz[vi]*ss, wz=vx[vi]*ss + vz[vi]*cs, wy=vy[vi];
                    float tt=(wy-FY)/ly;
                    S[j]=project(wx - tt*lx, FY, wz - tt*lz, 0,1,0, 0,0, ccy,scy, cxp,sxp, dist);
                }
                raster(S[0],S[1],S[2]);
            }
            shadowpass = 0;
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

        /* --- orbiting satellites: a little "orrery" of tinted spheres on tilted
         *     orbits, each lit + textured + z-buffered against everything else --- */
        {
            /* radius, orbitSpeed, phase, inclination, baseHeight, scale */
            static const float sat[][6] = {
                { 1.95f, 1.7f, 0.0f,  0.10f, 0.45f, 0.32f },
                { 2.70f, 1.1f, 2.1f,  0.45f,-0.10f, 0.22f },
                { 1.45f, 2.6f, 4.0f, -0.35f, 0.80f, 0.18f },
                { 3.20f, 0.8f, 1.0f,  0.22f, 0.10f, 0.28f },
            };
            static const int sattint[][3] = {
                {256,256,256}, {256,150,110}, {120,180,256}, {150,256,160},
            };
            int NS = (int)(sizeof(sat)/sizeof(sat[0]));
            float ccy=fcos(camYaw), scy=fsin(camYaw);
            for (int s=0; s<NS; s++) {
                float radius=sat[s][0], ospeed=sat[s][1], phase=sat[s][2];
                float incl=sat[s][3], hgt=sat[s][4], scl=sat[s][5];
                float oa=spin*ospeed + phase, ci=fcos(incl), si=fsin(incl);
                float px=radius*fcos(oa), pz0=radius*fsin(oa);            /* orbit in its own plane */
                float ocx=px, ocy=hgt - si*pz0, ocz=ci*pz0;              /* tilt about X -> depth */
                float ms=spin*(ospeed+1.3f), mc=fcos(ms), msn=fsin(ms);  /* self-spin */
                tintR=sattint[s][0]; tintG=sattint[s][1]; tintB=sattint[s][2];
                for (int i=0; i<mnv; i++) {
                    float sx=mvx[i]*scl, syv=mvy[i]*scl, sz=mvz[i]*scl;
                    float wx=ocx + sx*mc - sz*msn, wz=ocz + sx*msn + sz*mc, wy=ocy + syv;
                    float wnx=mnx[i]*mc - mnz[i]*msn, wnz=mnx[i]*msn + mnz[i]*mc;
                    sv2[i]=project(wx,wy,wz, wnx,mny[i],wnz, mtu[i],mtv[i], ccy,scy, cxp,sxp, dist);
                }
                for (int t=0; t<mnt; t++) {
                    SV A=sv2[mtri[t][0]], B=sv2[mtri[t][1]], C=sv2[mtri[t][2]];
                    float cross=(B.x-A.x)*(C.y-A.y)-(B.y-A.y)*(C.x-A.x);
                    if (cross<=0) continue;                              /* backface */
                    if (A.iz>4.0f||B.iz>4.0f||C.iz>4.0f) continue;       /* near-plane: drop too-close tris */
                    raster(A,B,C);
                }
            }
            tintR=tintG=tintB=256;   /* restore identity for the next frame's floor + model */
        }

        sys_gfx_blit(fb);
        sys_sleep(16);
    }
    free(fb); free(zbuf); free(tex); free(ftex);
    return 0;
}
