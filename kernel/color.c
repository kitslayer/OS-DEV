/* color.c — CSS colour parsing over untrusted page bytes.
 *
 * Split out of browser.c (M581). parse_color reads a colour token from arbitrary
 * page CSS / style attributes; every read is bounded by the slice length vl, and
 * the rgb/hsl integer conversions are clamped so a hostile value can't overflow.
 * Pure (no browser state) — fuzzed by tests/color. */
#include "color.h"
#include "htmlattr.h"   /* lc() — ASCII lowercase */

static int hexd(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c); if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
/* local equality test (browser.c keeps its own widely-used streqs) */
static int streqs(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; } return *a == *b;
}
/* integer HSL->RGB (no FPU): h 0..359, s/l 0..100 -> r/g/b 0..255 */
static void hsl_to_rgb(int h, int s, int l, int *r, int *g, int *b) {
    long L = (long)l * 255 / 100;
    long d1 = 2*L - 255; if (d1 < 0) d1 = -d1;
    long C = (255 - d1) * s / 100;                       /* chroma */
    long hp = (long)h * 100 / 60, m2 = hp % 200, d2 = m2 - 100; if (d2 < 0) d2 = -d2;
    long X = C * (100 - d2) / 100;
    long m = L - C/2, rr, gg, bb;
    int seg = h / 60;
    if      (seg == 0) { rr=C; gg=X; bb=0; }
    else if (seg == 1) { rr=X; gg=C; bb=0; }
    else if (seg == 2) { rr=0; gg=C; bb=X; }
    else if (seg == 3) { rr=0; gg=X; bb=C; }
    else if (seg == 4) { rr=X; gg=0; bb=C; }
    else               { rr=C; gg=0; bb=X; }
    rr+=m; gg+=m; bb+=m;
    if(rr<0)rr=0; if(rr>255)rr=255; if(gg<0)gg=0; if(gg>255)gg=255; if(bb<0)bb=0; if(bb>255)bb=255;
    *r=(int)rr; *g=(int)gg; *b=(int)bb;
}

uint32_t parse_color(const char *v, int vl) {
    if (vl <= 0) return 0;
    if (v[0] == '#') {
        int d[6], n = 0;
        for (int i = 1; i < vl && n < 6; i++) { int hx = hexd(v[i]); if (hx < 0) break; d[n++] = hx; }
        uint32_t rgb;
        if (n >= 6)      rgb = ((uint32_t)(d[0]*16+d[1])<<16) | ((d[2]*16+d[3])<<8) | (d[4]*16+d[5]);
        else if (n >= 3) rgb = ((uint32_t)(d[0]*17)<<16) | ((d[1]*17)<<8) | (d[2]*17);
        else return 0;
        return 0x01000000u | rgb;
    }
    if (vl >= 4 && lc(v[0])=='r' && lc(v[1])=='g' && lc(v[2])=='b') {   /* rgb(r,g,b) / rgba(r,g,b,a) — alpha ignored */
        int i = 3; if (i < vl && lc(v[i])=='a') i++;          /* skip the 'a' of rgba */
        while (i < vl && v[i] != '(') i++;
        if (i >= vl) return 0;
        i++;                                                  /* past '(' */
        int comp[3], nc = 0;
        while (i < vl && nc < 3) {
            while (i < vl && (v[i]==' '||v[i]==',')) i++;      /* skip separators */
            if (i >= vl || v[i] < '0' || v[i] > '9') break;   /* no number here (')' / '%'-only / junk) */
            int val = 0;
            while (i < vl && v[i] >= '0' && v[i] <= '9') { val = val*10 + (v[i]-'0'); if (val > 1000) val = 1000; i++; }
            if (i < vl && v[i]=='%') { val = val * 255 / 100; i++; }   /* percentage component */
            if (val > 255) val = 255;
            comp[nc++] = val;
            while (i < vl && v[i] != ',' && v[i] != ')') i++; /* to the next separator */
        }
        if (nc >= 3) return 0x01000000u | ((uint32_t)comp[0]<<16) | ((uint32_t)comp[1]<<8) | (uint32_t)comp[2];
        return 0;
    }
    if (vl >= 4 && lc(v[0])=='h' && lc(v[1])=='s' && lc(v[2])=='l') {   /* hsl(h,s%,l%) / hsla(...) — alpha ignored */
        int i = 3; if (i < vl && lc(v[i])=='a') i++;          /* skip the 'a' of hsla */
        while (i < vl && v[i] != '(') i++;
        if (i >= vl) return 0;
        i++;                                                  /* past '(' */
        int hsl[3], nc = 0;
        while (i < vl && nc < 3) {
            while (i < vl && (v[i]==' '||v[i]==',')) i++;      /* skip separators */
            int neg = 0; if (i < vl && v[i]=='-') { neg = 1; i++; }
            if (i >= vl || v[i] < '0' || v[i] > '9') break;
            int val = 0;
            while (i < vl && v[i] >= '0' && v[i] <= '9') { val = val*10 + (v[i]-'0'); if (val > 100000) val = 100000; i++; }
            if (i < vl && v[i]=='%') i++;                     /* s/l percent: keep 0..100 */
            hsl[nc++] = neg ? -val : val;
            while (i < vl && v[i] != ',' && v[i] != ')') i++;
        }
        if (nc >= 3) {
            int h = hsl[0] % 360; if (h < 0) h += 360;
            int s = hsl[1] < 0 ? 0 : hsl[1] > 100 ? 100 : hsl[1];
            int l = hsl[2] < 0 ? 0 : hsl[2] > 100 ? 100 : hsl[2];
            int r, g, bl; hsl_to_rgb(h, s, l, &r, &g, &bl);
            return 0x01000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)bl;
        }
        return 0;
    }
    static const struct { const char *n; uint32_t rgb; } named[] = {
        {"red",0xCC0000},{"green",0x008000},{"blue",0x0000CC},{"black",0x000000},
        {"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},{"orange",0xE07000},
        {"yellow",0xB8A000},{"purple",0x800080},{"navy",0x000080},{"teal",0x008080},
        {"maroon",0x800000},{"olive",0x808000},{"lime",0x00A000},{"cyan",0x008B8B},
        {"magenta",0xB000B0},{"brown",0x8B4513},{"pink",0xD06080},
        {"crimson",0xC0143C},{"gold",0xB8860B},{"indigo",0x4B0082},{"violet",0x8A2BE2},
        {"coral",0xD0522D},{"salmon",0xC0593B},{"turquoise",0x209888},{"tan",0x8B7355},
        {"darkred",0x8B0000},{"darkblue",0x00008B},{"darkgreen",0x006400},{"darkgray",0x595959},
        {"darkgrey",0x595959},{"slategray",0x5A6A78},{"slategrey",0x5A6A78},{"steelblue",0x3672A0},
        /* common CSS keywords that were missing — `white` especially is everywhere */
        {"white",0xFFFFFF},{"whitesmoke",0xF5F5F5},{"aqua",0x008B8B},{"fuchsia",0xB000B0},
        {"lightgray",0xD3D3D3},{"lightgrey",0xD3D3D3},{"lightblue",0xADD8E6},{"lightgreen",0x90C878},
        {"lightyellow",0xF8F8C0},{"skyblue",0x6CA6CD},{"hotpink",0xD0508A},{"khaki",0xBDB76B},
        {"beige",0xD8D0B8},{"chocolate",0xA0522D},{"firebrick",0xB02020},{"forestgreen",0x228B22},
        {"royalblue",0x4060D0},{"orchid",0xB060B0},{"sienna",0xA0522D},{"plum",0xC080C0},
    };
    char buf[16]; int p = 0;
    for (int i = 0; i < vl && p < 15; i++) buf[p++] = (char)lc(v[i]);
    buf[p] = 0;
    for (unsigned i = 0; i < sizeof(named)/sizeof(named[0]); i++)
        if (streqs(buf, named[i].n)) return 0x01000000u | named[i].rgb;
    return 0;
}
