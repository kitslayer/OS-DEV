/*
 * SVG rasterizer regression + fuzz test (host-side, ASan/UBSan).
 *
 * svg.c parses UNTRUSTED web XML in-kernel (no stack guard page) and rasterizes
 * it into an RGBA bitmap with integer-only (16.16 fixed-point) geometry. This
 * test (1) decodes hand-written SVGs and checks the pixels, (2) exercises a
 * <circle>+<path> bezier, and (3) fuzzes 100k+ random/truncated/mutated byte
 * buffers through svg_decode with bounded output/scratch buffers. A clean exit
 * is a pass; any OOB/overflow aborts under ASan/UBSan, and a non-terminating
 * decoder would hang. Build:
 *   gcc -std=gnu11 -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *       -Ikernel -Ikernel/include tests/svg/svg_test.c kernel/svg.c -o /tmp/svgtest
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int svg_decode(const uint8_t *data, int len, uint8_t *out, int out_cap,
               uint8_t *scratch, int scratch_cap, int *ow, int *oh);

/* Generous host buffers (BSS). The kernel passes a few hundred KB of scratch;
 * svg.c needs SVG_MAX_PTS*sizeof(fx)*2 = 8192*4*2 = 64 KB. Output is W*H*4 with
 * W,H<=512 -> up to 1 MB. */
static uint8_t obuf[512u*512u*4u];          /* 1 MB */
static uint8_t sbuf[256u*1024u];            /* 256 KB scratch */

/* svg.c uses the kernel font for <text>; stub it here (the real font is in font.c, linked
 * into the kernel build). 'A'/'B' are solid blocks so a <text> test can assert pixels;
 * other glyphs are blank. This locks draw_text's bounds + positioning, not glyph fidelity. */
#define SOLID16 {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}
const unsigned char font_glyphs[128][16] = { [0x41] = SOLID16, [0x42] = SOLID16 };

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static const uint8_t *px(int w, int x, int y) { return &obuf[((long)y*w + x)*4]; }

/* --- test 1: a red rect on a 32x32 transparent canvas -------------------- */
static void test_rect(void) {
    const char *svg = "<svg width=\"32\" height=\"32\">"
                      "<rect x=\"4\" y=\"4\" width=\"24\" height=\"24\" fill=\"#ff0000\"/></svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0, "rect: decode returned 0");
    CHECK(w == 32 && h == 32, "rect: W=32 H=32");
    if (r == 0 && w == 32 && h == 32) {
        const uint8_t *ctr = px(w, 16, 16);    /* center: inside the rect -> red, opaque */
        CHECK(ctr[0] > 200 && ctr[1] < 50 && ctr[2] < 50 && ctr[3] > 200,
              "rect: center pixel is opaque red");
        const uint8_t *cor = px(w, 0, 0);      /* corner: outside the rect -> transparent */
        CHECK(cor[3] == 0, "rect: corner pixel is transparent");
        const uint8_t *edge = px(w, 1, 1);     /* still outside the x>=4 rect */
        CHECK(edge[3] == 0, "rect: (1,1) transparent");
    }
}

/* --- test 2: viewBox scaling (16x16 viewBox -> 32x32 canvas) ------------- */
static void test_viewbox(void) {
    const char *svg = "<svg width='64' height='64' viewBox='0 0 16 16'>"
                      "<rect x='0' y='0' width='8' height='8' fill='blue'/></svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 64 && h == 64, "viewbox: decode 64x64");
    if (r == 0 && w == 64) {
        /* the 8x8 user rect scales by 4 -> covers pixels [0,32). (8,8) is blue. */
        const uint8_t *in = px(w, 8, 8);
        CHECK(in[2] > 200 && in[0] < 50 && in[3] > 200, "viewbox: (8,8) is blue");
        const uint8_t *out_ = px(w, 40, 40);   /* outside scaled rect */
        CHECK(out_[3] == 0, "viewbox: (40,40) transparent");
    }
}

/* --- test 3: a circle + a path with a cubic bezier ---------------------- */
static int count_nontransparent(int w, int h) {
    int n = 0;
    for (long i = 0; i < (long)w*h; i++) if (obuf[i*4+3] != 0) n++;
    return n;
}
static void test_circle_path(void) {
    const char *svg =
        "<svg width=\"100\" height=\"100\">"
        "<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"green\"/>"
        "<path d=\"M10 80 C 40 10, 65 10, 95 80 L 95 95 L 10 95 Z\" fill=\"#0000ff\"/>"
        "</svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 100 && h == 100, "circle+path: decode 100x100");
    if (r == 0) {
        /* The path's bezier top arches up to ~y=27 near x=50, so it overpaints
         * the circle below it. (50,25) is ABOVE the arch -> still green circle;
         * (50,50) is INSIDE the path -> blue. This proves the circle fill, the
         * path fill, AND the cubic-bezier flattening (the arch shape). */
        const uint8_t *g = px(w, 50, 25);      /* green circle, above the path */
        CHECK(g[1] > 100 && g[0] < 60 && g[3] > 200, "circle+path: circle filled green");
        const uint8_t *bl = px(w, 50, 50);     /* inside the bezier path -> blue */
        CHECK(bl[2] > 200 && bl[0] < 60 && bl[3] > 200, "circle+path: bezier path filled blue");
        const uint8_t *above = px(w, 50, 5);   /* above everything -> transparent */
        CHECK(above[3] == 0, "circle+path: top pixel transparent (bezier arch below it)");
        int nt = count_nontransparent(w, h);
        CHECK(nt > 500, "circle+path: many non-transparent pixels");
        printf("  (circle+path: %d non-transparent pixels)\n", nt);
    }
}

/* --- test 4: a polygon + a rounded rect + stroke ------------------------ */
static void test_polygon_stroke(void) {
    const char *svg =
        "<svg width='80' height='80'>"
        "<polygon points='40,5 75,75 5,75' style='fill:orange;stroke:black;stroke-width:2'/>"
        "<rect x='10' y='10' width='20' height='20' rx='5' fill='rgb(10,200,30)'/>"
        "</svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 80 && h == 80, "polygon+rrect: decode 80x80");
    if (r == 0) {
        const uint8_t *tri = px(w, 40, 60);    /* inside the triangle -> orange */
        CHECK(tri[3] > 200, "polygon+rrect: triangle interior filled");
        const uint8_t *rr = px(w, 20, 20);     /* inside rounded rect -> green-ish */
        CHECK(rr[1] > 100 && rr[3] > 200, "polygon+rrect: rounded rect filled");
    }
}

/* --- test 5: only-viewBox sizing, named colors, defs skipped ------------ */
static void test_misc(void) {
    const char *svg =
        "<?xml version='1.0'?>"
        "<!-- a comment --> "
        "<svg viewBox='0 0 50 50' xmlns='http://www.w3.org/2000/svg'>"
        "<defs><linearGradient id='g'/></defs>"
        "<rect x='0' y='0' width='50' height='50' fill='white'/>"
        "<circle cx='25' cy='25' r='10'/>"            /* default fill = black */
        "</svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 50 && h == 50, "misc: viewBox-only sizing 50x50");
    if (r == 0) {
        const uint8_t *bg = px(w, 2, 2);        /* white bg */
        CHECK(bg[0] > 200 && bg[1] > 200 && bg[2] > 200, "misc: white background");
        const uint8_t *dot = px(w, 25, 25);     /* black default-fill circle */
        CHECK(dot[0] < 50 && dot[1] < 50 && dot[2] < 50 && dot[3] > 200, "misc: default-black circle");
    }
}

/* --- test 6: transforms (translate/scale/rotate, <g> groups + per-shape) -- */
static void test_transform(void) {
    const char *svg =
        "<svg width='100' height='100'>"
        "<g transform='translate(50,0)'>"
          "<rect x='0' y='0' width='10' height='10' fill='red'/>"     /* -> [50,60]x[0,10]   */
        "</g>"
        "<rect x='0' y='0' width='6' height='6' fill='blue'/>"        /* CTM restored: origin */
        "<g transform='scale(2)'>"
          "<rect x='5' y='5' width='5' height='5' fill='lime'/>"      /* -> [10,20]x[10,20]   */
        "</g>"
        "<rect x='0' y='0' width='10' height='10' fill='green' transform='translate(40,40)'/>"  /* ->[40,50]x[40,50] */
        "<g transform='translate(20,20)'><g transform='translate(20,0)'>"
          "<rect x='0' y='0' width='8' height='8' fill='#ff00ff'/>"   /* nested -> [40,48]x[20,28] */
        "</g></g>"
        "<g transform='rotate(90,50,50)'>"
          "<rect x='50' y='40' width='20' height='4' fill='cyan'/>"   /* (x,y)->(100-y,x): -> x[56,60]y[50,70] */
        "</g>"
        "</svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 100 && h == 100, "transform: decode 100x100");
    if (r == 0) {
        const uint8_t *tr = px(w, 55, 5);    /* <g> translate moved the red rect here */
        CHECK(tr[0]>200 && tr[1]<60 && tr[2]<60 && tr[3]>200, "transform: <g> translate -> red at (55,5)");
        const uint8_t *og = px(w, 8, 8);     /* red rect's untranslated footprint [0,10], outside the blue [0,6] */
        CHECK(og[3]==0, "transform: red rect no longer at original (8,8)");
        const uint8_t *bl = px(w, 3, 3);     /* sibling after </g> is back at the origin */
        CHECK(bl[2]>200 && bl[0]<60 && bl[3]>200, "transform: CTM restored after </g> (blue at origin)");
        const uint8_t *sc = px(w, 15, 15);   /* scale(2): user (5..10) -> px (10..20) */
        CHECK(sc[1]>200 && sc[0]<60 && sc[3]>200, "transform: scale(2) -> lime at (15,15)");
        const uint8_t *so = px(w, 7, 7);     /* scaled rect starts at px 10, so (7,7) is empty */
        CHECK(so[3]==0, "transform: scaled rect does not cover (7,7)");
        const uint8_t *ps = px(w, 45, 45);   /* per-shape transform= on the rect */
        CHECK(ps[1]>100 && ps[0]<60 && ps[3]>200, "transform: per-shape translate -> green at (45,45)");
        const uint8_t *ne = px(w, 44, 24);   /* two nested <g> translates compose */
        CHECK(ne[0]>200 && ne[1]<60 && ne[2]>200 && ne[3]>200, "transform: nested <g> -> magenta at (44,24)");
        const uint8_t *ro = px(w, 58, 60);   /* rotate(90) about (50,50) maps the bar here */
        CHECK(ro[1]>200 && ro[2]>200 && ro[0]<60 && ro[3]>200, "transform: rotate(90) -> cyan at (58,60)");
        const uint8_t *rq = px(w, 62, 42);   /* ...and NOT at the bar's pre-rotation location */
        CHECK(rq[3]==0, "transform: rotate moved the bar off its original spot (62,42)");
    }
}

/* --- test 7: paint inheritance (<svg>/<g> fill, override, "inherit") ----- */
static void test_inherit(void) {
    const char *svg =
        "<svg width='100' height='100' fill='#ff0000'>"               /* root fill = red */
        "<rect x='0' y='0' width='20' height='20'/>"                  /* inherits red */
        "<g fill='#0000ff'>"                                          /* group fill = blue */
          "<rect x='30' y='0' width='20' height='20'/>"               /* inherits blue */
          "<rect x='60' y='0' width='20' height='20' fill='#00ff00'/>"/* own green overrides */
          "<g fill='inherit'>"                                        /* explicit inherit -> blue */
            "<rect x='30' y='30' width='20' height='20'/>"
          "</g>"
        "</g>"
        "<rect x='0' y='60' width='20' height='20'/>"                 /* after </g>: back to red */
        "</svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 100 && h == 100, "inherit: decode 100x100");
    if (r == 0) {
        const uint8_t *a = px(w, 10, 10);   /* root <svg fill> inherited */
        CHECK(a[0]>200 && a[1]<60 && a[2]<60 && a[3]>200, "inherit: root <svg fill> -> red");
        const uint8_t *b2 = px(w, 40, 10);  /* <g fill> inherited */
        CHECK(b2[2]>200 && b2[0]<60 && b2[3]>200, "inherit: <g fill> -> blue");
        const uint8_t *c2 = px(w, 70, 10);  /* shape's own fill overrides the group */
        CHECK(c2[1]>200 && c2[0]<60 && c2[2]<60 && c2[3]>200, "inherit: own fill overrides -> green");
        const uint8_t *d = px(w, 40, 40);   /* fill='inherit' takes the group's blue */
        CHECK(d[2]>200 && d[0]<60 && d[3]>200, "inherit: fill='inherit' -> blue");
        const uint8_t *e2 = px(w, 10, 70);  /* inherited paint restored after </g> */
        CHECK(e2[0]>200 && e2[1]<60 && e2[2]<60 && e2[3]>200, "inherit: paint restored after </g> -> red");
    }
}

/* --- test 8: opacity (fill-opacity / opacity / group opacity) ----------- */
static void test_opacity(void) {
    const char *svg =
        "<svg width='60' height='60'>"
        "<rect x='0' y='0' width='20' height='20' fill='red' fill-opacity='0.5'/>"     /* a~128 */
        "<rect x='20' y='0' width='20' height='20' fill='blue' opacity='0.25'/>"        /* a~64  */
        "<rect x='40' y='0' width='20' height='20' fill='black'/>"                      /* a=255 (additive) */
        "<g opacity='0.5'>"
          "<rect x='0' y='20' width='20' height='20' fill='green'/>"                    /* a~128 (group) */
          "<rect x='20' y='20' width='20' height='20' fill='green' opacity='0.5'/>"     /* a~64 (group*own) */
        "</g>"
        "<rect x='0' y='40' width='20' height='20' fill='black'/>"                      /* a=255 (in_alpha restored after </g>) */
        "</svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 60 && h == 60, "opacity: decode 60x60");
    if (r == 0) {
        const uint8_t *fo = px(w, 10, 10);   /* fill-opacity 0.5 over transparent -> ~half alpha */
        CHECK(fo[3] > 100 && fo[3] < 160 && fo[0] > 180, "opacity: fill-opacity 0.5 -> ~half-alpha red");
        const uint8_t *op = px(w, 30, 10);   /* opacity 0.25 -> ~quarter alpha */
        CHECK(op[3] > 40 && op[3] < 90 && op[2] > 180, "opacity: opacity 0.25 -> ~quarter-alpha blue");
        const uint8_t *opq = px(w, 50, 10);  /* no opacity attr -> fully opaque (additive) */
        CHECK(opq[3] > 250, "opacity: no opacity attr -> fully opaque");
        const uint8_t *go = px(w, 10, 30);   /* <g opacity=0.5> -> ~half alpha */
        CHECK(go[3] > 100 && go[3] < 160 && go[1] > 100, "opacity: group opacity 0.5 -> ~half-alpha green");
        const uint8_t *gc = px(w, 30, 30);   /* group 0.5 * own 0.5 -> ~quarter alpha */
        CHECK(gc[3] > 40 && gc[3] < 90, "opacity: group*element opacity -> ~quarter alpha");
        const uint8_t *rst = px(w, 10, 50);  /* after </g>: in_alpha restored -> opaque */
        CHECK(rst[3] > 250, "opacity: group opacity restored after </g>");
    }
}

/* --- test 9: gradients (linear red->blue + radial white->black) --------- */
static void test_gradient(void) {
    const char *svg =
        "<svg width='100' height='40'>"
        "<defs>"
          "<linearGradient id='lg'>"
            "<stop offset='0' stop-color='#ff0000'/>"
            "<stop offset='1' stop-color='#0000ff'/>"
          "</linearGradient>"
          "<radialGradient id='rg'>"
            "<stop offset='0' stop-color='#ffffff'/>"
            "<stop offset='1' stop-color='#000000'/>"
          "</radialGradient>"
        "</defs>"
        "<rect x='0' y='0' width='60' height='40' fill='url(#lg)'/>"   /* horizontal red->blue */
        "<circle cx='80' cy='20' r='18' fill='url(#rg)'/>"             /* radial white->black */
        "</svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 100 && h == 40, "gradient: decode 100x40");
    if (r == 0) {
        const uint8_t *lft = px(w, 3, 20);    /* linear t~0 -> red */
        CHECK(lft[0] > 180 && lft[2] < 80 && lft[3] > 200, "gradient: linear left edge ~red");
        const uint8_t *rgt = px(w, 56, 20);   /* linear t~1 -> blue */
        CHECK(rgt[2] > 180 && rgt[0] < 80 && rgt[3] > 200, "gradient: linear right edge ~blue");
        const uint8_t *mid = px(w, 30, 20);   /* linear t~0.5 -> blend (both channels mid) */
        CHECK(mid[0] > 60 && mid[0] < 200 && mid[2] > 60 && mid[2] < 200, "gradient: linear midpoint blends");
        const uint8_t *cen = px(w, 80, 20);   /* radial t~0 -> white */
        CHECK(cen[0] > 200 && cen[1] > 200 && cen[2] > 200 && cen[3] > 200, "gradient: radial center ~white");
        const uint8_t *edg = px(w, 80, 4);    /* radial t~0.9 (near r) -> dark */
        CHECK(edg[0] < 100 && edg[3] > 200, "gradient: radial edge ~dark");
    }
}

/* --- test 10: <text> (renders glyphs via the font; stub: 'A'/'B' = solid blocks) -- */
static void test_text(void) {
    const char *svg = "<svg width='100' height='40'>"
        "<text x='10' y='30' font-size='20' fill='#ff0000'>AB</text></svg>";
    int w, h;
    int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                       obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    CHECK(r == 0 && w == 100 && h == 40, "text: decode 100x40");
    if (r == 0) {
        const uint8_t *g = px(w, 15, 22);    /* inside the 'A' glyph cell ([10,20]x[10,30]) */
        CHECK(g[0] > 180 && g[1] < 60 && g[2] < 60 && g[3] > 200, "text: glyph drawn red at (15,22)");
        const uint8_t *o = px(w, 70, 35);    /* well outside the two glyphs */
        CHECK(o[3] == 0, "text: empty area stays transparent");
    }
}

/* --- test 11: <use>/<symbol> element reuse ------------------------------- */
static void test_use(void) {
    /* (a) a <g> def in <defs>, instantiated at an offset via href="#id". The def
     *     itself (inside <defs>) must NOT render in place; the <use> renders it
     *     translated by (x,y). */
    {
        const char *svg =
            "<svg width='60' height='60'>"
            "<defs><g id='box'><rect x='0' y='0' width='10' height='10' fill='red'/></g></defs>"
            "<use href='#box' x='30' y='30'/>"
            "</svg>";
        int w, h;
        int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                           obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        CHECK(r == 0 && w == 60 && h == 60, "use: <g> def decode 60x60");
        if (r == 0) {
            const uint8_t *u = px(w, 35, 35);   /* used instance: rect[0,10] + (30,30) -> [30,40] */
            CHECK(u[0]>200 && u[1]<60 && u[2]<60 && u[3]>200, "use: instance drawn red at (35,35)");
            const uint8_t *o = px(w, 5, 5);     /* the def in <defs> must NOT render in place */
            CHECK(o[3]==0, "use: def location (5,5) stays empty");
            const uint8_t *e = px(w, 50, 50);   /* outside the translated instance */
            CHECK(e[3]==0, "use: (50,50) outside instance transparent");
        }
    }
    /* (b) a bare <symbol> def reused twice at two offsets (and xlink:href spelling). */
    {
        const char *svg =
            "<svg width='80' height='40'>"
            "<defs><symbol id='dot'><circle cx='5' cy='5' r='4' fill='blue'/></symbol></defs>"
            "<use xlink:href='#dot' x='10' y='10'/>"   /* -> center (15,15) */
            "<use href='#dot' x='50' y='10'/>"         /* -> center (55,15) */
            "</svg>";
        int w, h;
        int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                           obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        CHECK(r == 0 && w == 80 && h == 40, "use: <symbol> twice decode 80x40");
        if (r == 0) {
            const uint8_t *a = px(w, 15, 15);   /* first instance */
            CHECK(a[2]>200 && a[0]<60 && a[3]>200, "use: symbol instance 1 blue at (15,15)");
            const uint8_t *b = px(w, 55, 15);   /* second instance (xlink vs href both work) */
            CHECK(b[2]>200 && b[0]<60 && b[3]>200, "use: symbol instance 2 blue at (55,15)");
            const uint8_t *mid = px(w, 35, 20); /* between the two -> empty */
            CHECK(mid[3]==0, "use: gap between symbol instances transparent");
            const uint8_t *def = px(w, 5, 5);   /* symbol content not rendered in place */
            CHECK(def[3]==0, "use: <symbol> def not rendered in place");
        }
    }
    /* (c) a <use> referencing a shape that lives OUTSIDE <defs> (forward ref to a
     *     later element id). The original still renders at its own spot AND the
     *     <use> renders a translated copy; an undefined id renders nothing. */
    {
        const char *svg =
            "<svg width='60' height='60'>"
            "<rect id='sq' x='0' y='0' width='10' height='10' fill='green'/>"  /* renders at origin */
            "<use href='#sq' x='40' y='40'/>"                                  /* + a copy at (40,40) */
            "<use href='#nope' x='20' y='20'/>"                                /* undefined -> nothing */
            "</svg>";
        int w, h;
        int r = svg_decode((const uint8_t*)svg, (int)strlen(svg),
                           obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        CHECK(r == 0 && w == 60 && h == 60, "use: forward-ref decode 60x60");
        if (r == 0) {
            const uint8_t *orig = px(w, 5, 5);    /* original shape at its own location */
            CHECK(orig[1]>100 && orig[0]<60 && orig[3]>200, "use: original shape renders at origin");
            const uint8_t *copy = px(w, 45, 45);  /* translated copy */
            CHECK(copy[1]>100 && copy[0]<60 && copy[3]>200, "use: copy renders at (45,45)");
            const uint8_t *none = px(w, 25, 25);  /* undefined id -> nothing here */
            CHECK(none[3]==0, "use: undefined href renders nothing");
        }
    }
}

/* --- fuzz harness -------------------------------------------------------- */
static uint32_t rs = 0x5EED1234u;
static uint32_t xr(void) { rs ^= rs<<13; rs ^= rs>>17; rs ^= rs<<5; return rs; }

int main(void) {
    test_rect();
    test_viewbox();
    test_circle_path();
    test_polygon_stroke();
    test_misc();
    test_transform();
    test_inherit();
    test_opacity();
    test_gradient();
    test_text();
    test_use();

    /* Valid seeds to mutate during fuzzing. */
    const char *seeds[] = {
        "<svg width=\"32\" height=\"32\"><rect x=\"4\" y=\"4\" width=\"24\" height=\"24\" fill=\"#ff0000\"/></svg>",
        "<svg width='100' height='100'><circle cx='50' cy='50' r='30' fill='green'/>"
            "<path d='M10 80 C 40 10, 65 10, 95 80 Z' fill='#00f'/></svg>",
        "<svg viewBox='0 0 24 24'><path d='M12 2 L22 22 H2 Z'/></svg>",
        "<svg width='64' height='64'><polygon points='1,1 60,1 30,60' stroke='red' stroke-width='3' fill='none'/></svg>",
        "<svg width='40' height='40'><ellipse cx='20' cy='20' rx='18' ry='9' fill='rgb(255,128,0)'/></svg>",
        "<svg width='60' height='60'><g transform='translate(10,10) scale(2) rotate(30)'>"
            "<rect x='0' y='0' width='10' height='10' fill='red' transform='rotate(45,5,5)'/>"
            "<g transform='matrix(1,0.2,-0.2,1,5,5)'><circle cx='5' cy='5' r='4'/></g></g></svg>",
    };
    int nseeds = (int)(sizeof seeds / sizeof seeds[0]);

    uint8_t f[2048];
    const int ITERS = 100000;
    int w, h;

    /* 4a. pure random bytes */
    for (int i = 0; i < ITERS; i++) {
        int n = 1 + (int)(xr() % 400);
        for (int j = 0; j < n; j++) f[j] = (uint8_t)xr();
        /* sometimes prefix a real "<svg" so the body parser actually runs */
        if ((i & 1) && n >= 4) { f[0]='<'; f[1]='s'; f[2]='v'; f[3]='g'; }
        svg_decode(f, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    }

    /* 4b. mutated copies of valid seeds (flip/insert/truncate) */
    for (int i = 0; i < ITERS; i++) {
        const char *seed = seeds[i % nseeds];
        int sl = (int)strlen(seed);
        if (sl > (int)sizeof f) sl = (int)sizeof f;
        int n = sl;
        memcpy(f, seed, n);
        int muts = 1 + (int)(xr() % 8);
        for (int m = 0; m < muts; m++) {
            int op = xr() % 3;
            if (op == 0 && n > 0) f[xr() % n] = (uint8_t)xr();        /* flip a byte */
            else if (op == 1 && n > 4) n = (int)(xr() % n);           /* truncate */
            else if (n > 0) f[xr() % n] ^= (uint8_t)(1u << (xr()&7)); /* bit flip */
        }
        if (n < 0) n = 0;
        svg_decode(f, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
    }

    /* 4c. STRUCTURED fuzz: splice SVG fragments + long digit runs into
     *     semi-valid markup, across many seeds. This is what actually drives
     *     the number parser, the path command machine and the rasterizer (pure
     *     random bytes rarely get past the <svg> scan). It caught a real latent
     *     overflow in parse_num (a huge numeric attribute made `num << 16`
     *     overflow int64 — now capped). Must stay ASan/UBSan-clean. */
    {
        static const char *frag[] = {
            "<svg width='","viewBox='0 0 ","' height='","<path d='M"," C","z'/>",
            "<rect x='","<circle cx='","fill='#","' style='fill:red;stroke:",
            " L"," Q"," A"," H"," V","-",".","e","99999","<polygon points='",
            ",","/>","</svg>","<ellipse rx='","' r='","' cy='","   ",
            "<g transform='","translate(","scale(","rotate(","matrix(","skewX(",
            "skewY(",")","'>","</g>","' transform='rotate(",
            "<g fill='","' fill='","stroke='","' fill='inherit'","#abc","' stroke-width='",
            "' opacity='","' fill-opacity='","' stroke-opacity='","0.5","%","0.25",
            "<defs>","</defs>","<linearGradient id='","<radialGradient id='","</linearGradient>",
            "</radialGradient>","<stop offset='","' stop-color='","' stop-opacity='","fill='url(#",
            "url(#g)","' gradientUnits='userSpaceOnUse","' cx='","' r='","</stop>",
            "<text x='","' y='","' font-size='","'>","</text>","ABCabc 123 xyz!",
            /* <use>/<symbol> element-reuse fragments: ids, href/xlink:href, #refs,
             * self-referential + cyclic + nested -> must terminate (depth cap) and
             * never OOB on a missing/garbage id or an unterminated container. */
            "<use href='#","<use xlink:href='#","<symbol id='","</symbol>",
            "id='","' x='","' y='","#a","#b","#self","#missing","'/>","' href='#a'/>",
            "<g id='","<defs><g id='a'>","</g></defs>","<use href='#a' x='5' y='5'/>",
        };
        int nfr = (int)(sizeof frag / sizeof frag[0]);
        for (unsigned seed = 1; seed <= 16; seed++) {
            rs = seed * 2654435761u + 12345u;
            for (int i = 0; i < 20000; i++) {
                int n = 0, parts = 1 + (int)(xr() % 40);
                for (int pn = 0; pn < parts && n < (int)sizeof f - 32; pn++) {
                    if (xr() % 3 == 0) {                 /* a run of digits */
                        int d = 1 + (int)(xr() % 8);
                        while (d-- > 0 && n < (int)sizeof f - 1) f[n++] = '0' + (char)(xr()%10);
                        if (n < (int)sizeof f - 1) f[n++] = ' ';
                    } else {
                        const char *fr = frag[xr() % nfr];
                        for (int k = 0; fr[k] && n < (int)sizeof f - 1; k++) f[n++] = fr[k];
                    }
                }
                svg_decode(f, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
            }
        }
    }

    /* 4d. adversarial sizes / pathological d-strings (bounded, must not OOB) */
    {
        /* huge declared dims (must be capped / rejected, never overflow) */
        const char *big = "<svg width='99999' height='99999'><rect width='99999' height='99999' fill='red'/></svg>";
        svg_decode((const uint8_t*)big, (int)strlen(big), obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        /* gradient with HUGE coords on a HUGE shape (the grad_color_at dx*dx int64
         * overflow path — must clamp, never overflow/UB). */
        const char *bg = "<svg width='400' height='400' viewBox='0 0 999999 999999'>"
            "<defs><linearGradient id='g' gradientUnits='userSpaceOnUse' x1='-999999' y1='-999999' x2='999999' y2='999999'>"
            "<stop offset='0' stop-color='#f00'/><stop offset='1' stop-color='#00f'/></linearGradient>"
            "<radialGradient id='r' gradientUnits='userSpaceOnUse' cx='999999' cy='-999999' r='999999'>"
            "<stop offset='0' stop-color='#fff'/><stop offset='1' stop-color='#000'/></radialGradient></defs>"
            "<rect width='999999' height='999999' fill='url(#g)'/>"
            "<circle cx='500000' cy='500000' r='499999' fill='url(#r)'/></svg>";
        svg_decode((const uint8_t*)bg, (int)strlen(bg), obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        /* a path with thousands of commands */
        static char buf[16000];
        int n = 0;
        n += sprintf(buf+n, "<svg width='64' height='64'><path d='M0 0");
        for (int i = 0; i < 3000 && n < (int)sizeof buf - 32; i++)
            n += sprintf(buf+n, " L%d %d", (int)(xr()%200) - 50, (int)(xr()%200) - 50);
        n += sprintf(buf+n, " Z'/></svg>");
        svg_decode((const uint8_t*)buf, n, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        /* deeply nested / unterminated tags */
        const char *un = "<svg width='10' height='10'><rect x='0' y='0' width='5' height='5' fill='#abc'";
        svg_decode((const uint8_t*)un, (int)strlen(un), obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        /* <use> recursion must terminate via the depth cap (no infinite recursion): */
        /*  - a <g> that <use>s itself */
        const char *self = "<svg width='20' height='20'>"
            "<defs><g id='s'><rect x='0' y='0' width='4' height='4' fill='red'/>"
            "<use href='#s' x='2' y='2'/></g></defs><use href='#s' x='0' y='0'/></svg>";
        svg_decode((const uint8_t*)self, (int)strlen(self), obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        /*  - a mutually-recursive cycle a -> b -> a */
        const char *cyc = "<svg width='20' height='20'><defs>"
            "<g id='a'><rect width='4' height='4' fill='red'/><use href='#b' x='1' y='1'/></g>"
            "<g id='b'><rect width='4' height='4' fill='blue'/><use href='#a' x='1' y='1'/></g>"
            "</defs><use href='#a'/></svg>";
        svg_decode((const uint8_t*)cyc, (int)strlen(cyc), obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        /*  - a <use> referencing an UNTERMINATED container (find_def -> docend) */
        const char *udef = "<svg width='20' height='20'><defs><g id='g'>"
            "<rect width='5' height='5' fill='green'/><use href='#g' x='2' y='2'/>";
        svg_decode((const uint8_t*)udef, (int)strlen(udef), obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        /*  - a deeply NESTED <use> chain u0->u1->...->u9 (each id used by the prior) */
        {
            static char ub[4096]; int un2 = 0;
            un2 += sprintf(ub+un2, "<svg width='40' height='40'><defs>");
            for (int i = 0; i < 10; i++)
                un2 += sprintf(ub+un2, "<g id='u%d'><rect width='3' height='3' fill='red'/>"
                                       "<use href='#u%d' x='1' y='1'/></g>", i, i+1);
            un2 += sprintf(ub+un2, "</defs><use href='#u0' x='0' y='0'/></svg>");
            svg_decode((const uint8_t*)ub, un2, obuf, sizeof obuf, sbuf, sizeof sbuf, &w, &h);
        }
        /* tiny scratch -> must reject cleanly */
        int r2 = svg_decode((const uint8_t*)seeds[0], (int)strlen(seeds[0]),
                            obuf, sizeof obuf, sbuf, 16, &w, &h);
        CHECK(r2 == -1, "adversarial: tiny scratch rejected");
        /* tiny out_cap -> must reject cleanly */
        r2 = svg_decode((const uint8_t*)seeds[0], (int)strlen(seeds[0]),
                        obuf, 8, sbuf, sizeof sbuf, &w, &h);
        CHECK(r2 == -1, "adversarial: tiny out_cap rejected");
    }

    if (fails == 0)
        printf("svgtest: 11 unit tests (transforms + inheritance + opacity + gradients + text + use/symbol) + %d random + %d "
               "mutation + 320000 structured fuzz iters + adversarial cases — ASan/UBSan clean, PASS\n", ITERS, ITERS);
    else
        printf("svgtest: %d FAILURE(S)\n", fails);
    return fails ? 1 : 0;
}
