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

/* --- fuzz harness -------------------------------------------------------- */
static uint32_t rs = 0x5EED1234u;
static uint32_t xr(void) { rs ^= rs<<13; rs ^= rs>>17; rs ^= rs<<5; return rs; }

int main(void) {
    test_rect();
    test_viewbox();
    test_circle_path();
    test_polygon_stroke();
    test_misc();

    /* Valid seeds to mutate during fuzzing. */
    const char *seeds[] = {
        "<svg width=\"32\" height=\"32\"><rect x=\"4\" y=\"4\" width=\"24\" height=\"24\" fill=\"#ff0000\"/></svg>",
        "<svg width='100' height='100'><circle cx='50' cy='50' r='30' fill='green'/>"
            "<path d='M10 80 C 40 10, 65 10, 95 80 Z' fill='#00f'/></svg>",
        "<svg viewBox='0 0 24 24'><path d='M12 2 L22 22 H2 Z'/></svg>",
        "<svg width='64' height='64'><polygon points='1,1 60,1 30,60' stroke='red' stroke-width='3' fill='none'/></svg>",
        "<svg width='40' height='40'><ellipse cx='20' cy='20' rx='18' ry='9' fill='rgb(255,128,0)'/></svg>",
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
        printf("svgtest: 5 unit tests + %d random + %d mutation + 320000 structured fuzz iters "
               "+ adversarial cases — ASan/UBSan clean, PASS\n", ITERS, ITERS);
    else
        printf("svgtest: %d FAILURE(S)\n", fails);
    return fails ? 1 : 0;
}
