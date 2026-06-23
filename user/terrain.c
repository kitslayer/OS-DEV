/*
 * terrain.c — a procedural terrain flythrough for OS-DEV. No GPU.
 *
 * A from-scratch perspective renderer flying forward over an endless,
 * procedurally-generated heightmap (rolling hills + ridges from summed
 * sines), flat-shaded by a directional SUN, coloured by ALTITUDE
 * (deep water -> shallows -> sand -> grass -> rock -> snow), per-pixel
 * Z-BUFFERED, with distance FOG fading the far terrain into a gradient
 * sky. The grid is regenerated each frame in the camera's forward
 * direction, so the world is effectively infinite. Float math, so built
 * with SSE (the generic user rule is -mgeneral-regs-only); trig/sqrt are
 * tiny self-contained approximations, like scene3d.
 *
 * Controls: A/D or left/right = turn · W/S or up/down = look · +/- = speed
 *           SPACE = pause flight · Q/Esc = quit.
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
    for (int i = 0; i < 10; i++) r = 0.5f * (r + x / r);
    return r;
}

static unsigned *fb;       /* W*H 0x00RRGGBB framebuffer */
static float    *zbuf;     /* W*H depth as 1/z (LARGER = nearer) */
static float     focal;
#define NEARZ 0.25f

static float Lx,Ly,Lz;     /* normalized directional sun */

/* ---- the heightmap: smooth rolling hills + ridges from summed sines ------- */
static float terr_h(float x, float z) {
    float h = 0.0f;
    h += fsin(x*0.045f) * 7.0f;                 /* broad rolling base */
    h += fcos(z*0.039f) * 7.0f;
    h += fsin(x*0.11f + z*0.09f) * 3.0f;        /* medium hills */
    h += fcos(x*0.21f - z*0.17f) * 1.6f;        /* finer detail */
    h += fsin(x*0.5f + z*0.43f) * 0.5f;
    return h;
}

/* ---- altitude -> colour (water/sand/grass/rock/snow), lerped bands -------- */
static unsigned lerpc(int r0,int g0,int b0,int r1,int g1,int b1,float t){
    if (t<0)t=0; if (t>1)t=1;
    int r=(int)(r0+(r1-r0)*t), g=(int)(g0+(g1-g0)*t), b=(int)(b0+(b1-b0)*t);
    return ((unsigned)r<<16)|((unsigned)g<<8)|(unsigned)b;
}
static unsigned terr_col(float h) {
    if (h < -3.0f) return lerpc( 18, 38, 92,  28, 66,150, (h+9)/6.0f);     /* deep -> mid water */
    if (h <  0.0f) return lerpc( 28, 66,150, 200,188,128, (h+3)/3.0f);     /* water -> sand */
    if (h <  1.5f) return lerpc(200,188,128,  66,140, 52, (h)/1.5f);       /* sand -> grass */
    if (h <  6.0f) return lerpc( 66,140, 52,  92,116, 58, (h-1.5f)/4.5f);  /* grass shades */
    if (h < 10.0f) return lerpc( 92,116, 58, 120,112,102, (h-6)/4.0f);     /* grass -> rock */
    return                lerpc(120,112,102, 238,238,250, (h-10)/4.0f);    /* rock -> snow */
}

/* A screen vertex (flat shading needs only position + depth). */
typedef struct { float x, y, iz; int ok; } SV;

/* World -> camera (translate by eye, yaw about Y, pitch about right) -> screen. */
static SV project(float wx,float wy,float wz, float ex,float ey,float ez,
                  float cy,float sy,float cp,float sp) {
    float dx=wx-ex, dy=wy-ey, dz=wz-ez;
    float rx = dx*cy - dz*sy;          /* right axis */
    float fz = dx*sy + dz*cy;          /* forward axis */
    float uy2 =  dy*cp - fz*sp;        /* pitch about the right axis */
    float fz2 =  dy*sp + fz*cp;
    SV s;
    if (fz2 < NEARZ) { s.ok = 0; return s; }
    float iz = 1.0f/fz2;
    s.x = W*0.5f + rx*focal*iz;
    s.y = H*0.5f - uy2*focal*iz;
    s.iz = iz; s.ok = 1;
    return s;
}

static void raster(SV a, SV b, SV c, unsigned col) {
    SV t;
    if (b.y<a.y){t=a;a=b;b=t;} if (c.y<a.y){t=a;a=c;c=t;} if (c.y<b.y){t=b;b=c;c=t;}
    int y0=(int)(a.y+0.5f), y2=(int)(c.y+0.5f);
    if (y2<=y0) return;
    if (y0<0) y0=0; if (y2>H) y2=H;
    unsigned CR=(col>>16)&255, CG=(col>>8)&255, CB=col&255;
    for (int y=y0; y<y2; y++) {
        float fy=y+0.5f;
        float t1=(c.y==a.y)?0.0f:(fy-a.y)/(c.y-a.y);
        float xl=a.x+(c.x-a.x)*t1, izl=a.iz+(c.iz-a.iz)*t1;
        float xr,izr;
        if (fy<b.y){ float t2=(b.y==a.y)?0.0f:(fy-a.y)/(b.y-a.y); xr=a.x+(b.x-a.x)*t2; izr=a.iz+(b.iz-a.iz)*t2; }
        else       { float t2=(c.y==b.y)?0.0f:(fy-b.y)/(c.y-b.y); xr=b.x+(c.x-b.x)*t2; izr=b.iz+(c.iz-b.iz)*t2; }
        if (xr<xl){ float s; s=xl;xl=xr;xr=s; s=izl;izl=izr;izr=s; }
        int x0=(int)(xl+0.5f), x1=(int)(xr+0.5f);
        if (x0<0) x0=0; if (x1>W) x1=W;
        float span=(xr-xl); if (span<0.001f) span=0.001f;
        unsigned *row=fb+(long)y*W; float *zr=zbuf+(long)y*W;
        for (int x=x0; x<x1; x++) {
            float iz=izl+(izr-izl)*((x+0.5f-xl)/span);
            if (iz<=zr[x]) continue;       /* nearer = larger 1/z */
            zr[x]=iz;
            row[x]=((unsigned)CR<<16)|((unsigned)CG<<8)|(unsigned)CB;
        }
    }
}

/* ---- the forward-flying camera grid ---------------------------------------*/
#define GD 60          /* grid rows forward */
#define GW 56          /* grid columns sideways */
#define STEP 1.6f
static float gwx[(GD+1)*(GW+1)], gwz[(GD+1)*(GW+1)], gh[(GD+1)*(GW+1)];
static SV    gsv[(GD+1)*(GW+1)];

#define FOG_START 28.0f
#define FOG_END   (GD*STEP*0.95f)

/* per-frame camera + fog state, so the tree pass can reuse project()/lighting */
static float Cex,Cey,Cez,Ccy,Csy,Ccp,Csp;
static unsigned Cfog;

/* one flat-shaded, sun-lit, fogged triangle from world-space corners */
static void tri3d(float x0,float y0,float z0, float x1,float y1,float z1,
                  float x2,float y2,float z2, unsigned base) {
    SV A=project(x0,y0,z0, Cex,Cey,Cez, Ccy,Csy,Ccp,Csp);
    SV B=project(x1,y1,z1, Cex,Cey,Cez, Ccy,Csy,Ccp,Csp);
    SV C=project(x2,y2,z2, Cex,Cey,Cez, Ccy,Csy,Ccp,Csp);
    if (!A.ok||!B.ok||!C.ok) return;
    float ux=x1-x0,uy=y1-y0,uz=z1-z0, vx=x2-x0,vy=y2-y0,vz=z2-z0;
    float nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
    float nl=fsqrt(nx*nx+ny*ny+nz*nz); if(nl<1e-4f)nl=1e-4f;
    float diff=(nx*Lx+ny*Ly+nz*Lz)/nl; if(diff<0)diff=-diff;   /* two-sided (foliage) */
    float lit=0.38f+0.66f*diff;
    float izavg=(A.iz+B.iz+C.iz)*0.3333f, dist=(izavg>1e-5f)?1.0f/izavg:1e5f;
    float fog=(dist-FOG_START)/(FOG_END-FOG_START); if(fog<0)fog=0; if(fog>1)fog=1;
    int cr=(int)(((base>>16)&255)*lit), cg=(int)(((base>>8)&255)*lit), cb=(int)((base&255)*lit);
    cr+=(int)(((int)((Cfog>>16)&255)-cr)*fog); cg+=(int)(((int)((Cfog>>8)&255)-cg)*fog); cb+=(int)(((int)(Cfog&255)-cb)*fog);
    if(cr>255)cr=255; if(cg>255)cg=255; if(cb>255)cb=255;
    raster(A,B,C, ((unsigned)cr<<16)|((unsigned)cg<<8)|(unsigned)cb);
}

/* a simple pine: a 4-sided trunk column + a 6-sided conical canopy */
static void draw_tree(float bx, float bz, float ground, unsigned h) {
    float fh = 2.3f + ((h>>12)&7)*0.18f;       /* canopy height varies a little per tree */
    float t = 0.17f, fr = 1.05f, th = 1.0f;
    float ty = ground, top = ty+th, apex = top+fh;
    unsigned trunk = 0x6b4a2a;
    unsigned leaf  = ((h>>16)&1) ? 0x2f7d34 : 0x357f3c;   /* two greens for variety */
    float xs[4]={bx-t,bx+t,bx+t,bx-t}, zs[4]={bz-t,bz-t,bz+t,bz+t};
    for (int i=0;i<4;i++){ int j=(i+1)&3;
        tri3d(xs[i],ty,zs[i], xs[j],ty,zs[j], xs[j],top,zs[j], trunk);
        tri3d(xs[i],ty,zs[i], xs[j],top,zs[j], xs[i],top,zs[i], trunk);
    }
    for (int i=0;i<6;i++){
        float a0=2*PI*i/6, a1=2*PI*(i+1)/6;
        tri3d(bx+fr*fcos(a0),top,bz+fr*fsin(a0),
              bx+fr*fcos(a1),top,bz+fr*fsin(a1),
              bx,apex,bz, leaf);
    }
}

int main(void) {
    if (sys_gfx_init(W,H) < 0) { print("terrain: graphics init failed\n"); return 1; }
    fb   = malloc((unsigned long)W*H*4);
    zbuf = malloc((unsigned long)W*H*sizeof(float));
    if (!fb || !zbuf) { print("terrain: out of memory\n"); return 1; }
    sys_caret(0);

    focal = 1.25f * (H * 0.5f);
    Lx=0.5f; Ly=0.75f; Lz=-0.35f;
    float ll=fsqrt(Lx*Lx+Ly*Ly+Lz*Lz); Lx/=ll; Ly/=ll; Lz/=ll;

    float ex=0.0f, ez=0.0f, ey=0.0f;
    float yaw=0.0f, pitch=-0.32f;     /* look slightly down */
    float speed=0.55f, fly=1.0f;
    unsigned fogc = 0x9fb6cf;          /* horizon haze the far terrain fades into */

    for (;;) {
        int k = sys_pollkey();
        if (k=='q'||k=='Q'||k==27) break;
        if (k=='+'||k=='=') { speed+=0.15f; if(speed>2.5f)speed=2.5f; }
        if (k=='-'||k=='_') { speed-=0.15f; if(speed<0.0f)speed=0.0f; }
        if (k=='a'||k=='A') yaw -= 0.05f;
        if (k=='d'||k=='D') yaw += 0.05f;
        if (k=='w'||k=='W') pitch -= 0.04f;
        if (k=='s'||k=='S') pitch += 0.04f;
        if (k==' ') fly = fly>0.5f?0.0f:1.0f;
        int e;
        while ((e=sys_getkbevent())>=0) {     /* arrow keys (extended scancodes) */
            int sc=e&0xFF;
            if (e&0x200) { if(sc==0x4B)yaw-=0.05f; if(sc==0x4D)yaw+=0.05f;
                           if(sc==0x48)pitch-=0.04f; if(sc==0x50)pitch+=0.04f; }
        }
        if (pitch<-0.9f)pitch=-0.9f; if (pitch>0.4f)pitch=0.4f;

        float cy=fcos(yaw), sy=fsin(yaw), cp=fcos(pitch), sp=fsin(pitch);
        float fwx=sy, fwz=cy, rgx=cy, rgz=-sy;   /* forward + right (world XZ) */

        /* fly forward; the camera floats a fixed height above the ground */
        ex += fwx*speed*fly; ez += fwz*speed*fly;
        float ground = terr_h(ex,ez);
        float want = ground + 9.0f;
        ey += (want - ey) * 0.12f;               /* smooth altitude follow */

        /* --- skybox: vertical gradient + a low sun, clears z (1/z=0 farthest) --- */
        for (int y=0; y<H; y++) {
            int gt = y*256/H;
            int sr = 30 + gt*120/256, sg = 60 + gt*120/256, sb = 110 + gt*70/256;
            int dys = y - H/3;
            unsigned *row=fb+(long)y*W; float *zr=zbuf+(long)y*W;
            for (int x=0; x<W; x++) {
                int r=sr,g=sg,b=sb;
                int dxs=x-(W*3/5), d2=dxs*dxs+dys*dys;     /* soft sun glow */
                if (d2 < 9000) { int gl=(9000-d2)*170/9000; r+=gl; g+=(gl*9)/10; b+=(gl*6)/10; }
                if (r>255)r=255; if(g>255)g=255; if(b>255)b=255;
                row[x]=((unsigned)r<<16)|((unsigned)g<<8)|(unsigned)b;
                zr[x]=0.0f;
            }
        }

        /* --- build the forward-oriented grid: world pos + height + projection --- */
        int W1=GW+1, half=GW/2;
        for (int d=0; d<=GD; d++) {
            for (int s=0; s<=GW; s++) {
                float fd=(float)d*STEP, sd=(float)(s-half)*STEP;
                float wx=ex + fwx*fd + rgx*sd;
                float wz=ez + fwz*fd + rgz*sd;
                float hh=terr_h(wx,wz);
                int idx=d*W1+s;
                gwx[idx]=wx; gwz[idx]=wz; gh[idx]=hh;
                gsv[idx]=project(wx,hh,wz, ex,ey,ez, cy,sy,cp,sp);
            }
        }

        /* --- rasterize each cell as two flat-shaded, fogged triangles --- */
        for (int d=0; d<GD; d++) {
            for (int s=0; s<GW; s++) {
                int i00=d*W1+s, i10=d*W1+s+1, i01=(d+1)*W1+s, i11=(d+1)*W1+s+1;
                SV p00=gsv[i00], p10=gsv[i10], p01=gsv[i01], p11=gsv[i11];
                if (!p00.ok||!p10.ok||!p01.ok||!p11.ok) continue;   /* clip cells crossing near plane */

                /* one normal per cell (world space); +Y up */
                float ax=gwx[i10]-gwx[i00], ay=gh[i10]-gh[i00], az=gwz[i10]-gwz[i00];
                float bx=gwx[i01]-gwx[i00], by=gh[i01]-gh[i00], bz=gwz[i01]-gwz[i00];
                float nx=by*az-bz*ay, ny=bz*ax-bx*az, nz=bx*ay-by*ax;
                float nl=fsqrt(nx*nx+ny*ny+nz*nz); if (nl<1e-4f) nl=1e-4f;
                float diff=(nx*Lx+ny*Ly+nz*Lz)/nl; if (diff<0)diff=0;
                float lit=0.32f + 0.78f*diff;

                /* fog by the cell's forward distance (1/iz averaged) */
                float izavg=(p00.iz+p10.iz+p01.iz+p11.iz)*0.25f;
                float dist=(izavg>1e-5f)?1.0f/izavg:1e5f;
                float fog=(dist-FOG_START)/(FOG_END-FOG_START); if(fog<0)fog=0; if(fog>1)fog=1;

                float havg=(gh[i00]+gh[i10]+gh[i01])*0.3333f;
                unsigned base=terr_col(havg);
                int R=(int)(((base>>16)&255)*lit), G=(int)(((base>>8)&255)*lit), B=(int)((base&255)*lit);
                R=(int)(R+( (int)((fogc>>16)&255)-R)*fog);
                G=(int)(G+( (int)((fogc>>8)&255)-G)*fog);
                B=(int)(B+( (int)(fogc&255)-B)*fog);
                if(R>255)R=255; if(G>255)G=255; if(B>255)B=255;
                unsigned c1=((unsigned)R<<16)|((unsigned)G<<8)|(unsigned)B;

                /* second triangle uses its own avg height for a smoother palette */
                float havg2=(gh[i10]+gh[i11]+gh[i01])*0.3333f;
                unsigned base2=terr_col(havg2);
                int R2=(int)(((base2>>16)&255)*lit), G2=(int)(((base2>>8)&255)*lit), B2=(int)((base2&255)*lit);
                R2=(int)(R2+( (int)((fogc>>16)&255)-R2)*fog);
                G2=(int)(G2+( (int)((fogc>>8)&255)-G2)*fog);
                B2=(int)(B2+( (int)(fogc&255)-B2)*fog);
                if(R2>255)R2=255; if(G2>255)G2=255; if(B2>255)B2=255;
                unsigned c2=((unsigned)R2<<16)|((unsigned)G2<<8)|(unsigned)B2;

                raster(p00,p10,p01, c1);
                raster(p10,p11,p01, c2);
            }
        }

        /* --- scatter low-poly pines on grassy ground (stable world-lattice
         *     placement so they don't swim as the camera flies; z-buffered
         *     against the terrain so hills hide the trees behind them) --- */
        Cex=ex;Cey=ey;Cez=ez;Ccy=cy;Csy=sy;Ccp=cp;Csp=sp;Cfog=fogc;
        {
            float SP=5.5f;                              /* tree lattice spacing */
            int ci=(int)(ex/SP), cj=(int)(ez/SP), rad=(int)(GD*STEP*0.8f/SP);
            for (int dj=-rad; dj<=rad; dj++)
                for (int di=-rad; di<=rad; di++) {
                    int li=ci+di, lj=cj+dj;
                    unsigned h=(unsigned)(li*73856093) ^ (unsigned)(lj*19349663);
                    h^=h>>13; h*=0x5bd1e995u; h^=h>>15;
                    if ((h&3u)!=0) continue;            /* ~1 in 4 cells has a tree */
                    float jx=(((h>>4)&15)/15.0f-0.5f)*SP*0.7f, jz=(((h>>8)&15)/15.0f-0.5f)*SP*0.7f;
                    float wx=(li+0.5f)*SP+jx, wz=(lj+0.5f)*SP+jz;
                    float rel=(wx-ex)*sy+(wz-ez)*cy;    /* forward distance in camera frame */
                    if (rel < -3.0f) continue;          /* behind the camera */
                    float g=terr_h(wx,wz);
                    if (g<0.4f || g>7.0f) continue;     /* grassy ground only (no water/snow) */
                    draw_tree(wx, wz, g, h);
                }
        }

        sys_gfx_blit(fb);
        sys_sleep(16);
    }
    free(fb); free(zbuf);
    return 0;
}
