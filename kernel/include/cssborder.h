/* cssborder.h — the CSS `border` shorthand value parser, extracted from browser.c
 * (M1777) so the untrusted border value (arbitrary page <style>/inline style) can be
 * host-tested for conformance + bounds, the codebase's extract-for-testability pattern
 * (see cssel.h / cssprop.c / color.c / url.c). Header-only static (like cssel.h): the one
 * kernel user is browser.c; the host test (tests/css) drives it alongside the real
 * style_prop + parse_color. Self-contained (manual token compares, no <string.h>). */
#ifndef CSSBORDER_H
#define CSSBORDER_H
#include "cssprop.h"   /* style_prop */
#include "color.h"     /* parse_color (+ uint32_t) */

/* CSS border. Returns (width<<28)|(sides<<24)|color, 0 if none. `sides` is a bitmask
 * (1=top 2=right 4=bottom 8=left, 15=all from the `border` shorthand). Pulls a px width
 * and a colour from the value; defaults 1px / grey. style_prop matches each property name
 * up to ':' exactly, so "border" never matches "border-top". One width/colour per box.
 * A `border-style: none`/`hidden` token or a zero width (`0`/`0px`) yields 0 = NO border
 * (must not fall through to the 1px grey default) — M1777, CSS conformance survey F2. */
static uint32_t parse_style_border(const char *s, int n) {
    int vs, ve, sides = 0, vstart = -1, vend = -1;
    if (style_prop(s, n, "border", 6, &vs, &ve))         { sides  = 15; vstart = vs; vend = ve; }
    if (style_prop(s, n, "border-top", 10, &vs, &ve))    { sides |= 1;  if (vstart < 0) { vstart = vs; vend = ve; } }
    if (style_prop(s, n, "border-right", 12, &vs, &ve))  { sides |= 2;  if (vstart < 0) { vstart = vs; vend = ve; } }
    if (style_prop(s, n, "border-bottom", 13, &vs, &ve)) { sides |= 4;  if (vstart < 0) { vstart = vs; vend = ve; } }
    if (style_prop(s, n, "border-left", 11, &vs, &ve))   { sides |= 8;  if (vstart < 0) { vstart = vs; vend = ve; } }
    if (!sides || vstart < 0) return 0;
    const char *v = s + vstart; int vl = vend - vstart;
    /* none/hidden style, or a zero width -> no border at all (F2) */
    for (int i = 0; i < vl; ) {
        while (i < vl && (v[i]==' '||v[i]=='\t')) i++;
        int ts = i; while (i < vl && v[i]!=' ' && v[i]!='\t') i++;
        int tl = i - ts; if (tl <= 0) continue;
        if ((tl==4 && v[ts]=='n'&&v[ts+1]=='o'&&v[ts+2]=='n'&&v[ts+3]=='e') ||
            (tl==6 && v[ts]=='h'&&v[ts+1]=='i'&&v[ts+2]=='d'&&v[ts+3]=='d'&&v[ts+4]=='e'&&v[ts+5]=='n')) return 0;
        if (v[ts] >= '0' && v[ts] <= '9') { int w=0; for (int j=ts; j<i && v[j]>='0'&&v[j]<='9'; j++) w=w*10+(v[j]-'0'); if (w==0) return 0; }
    }
    int width = 1;
    for (int i = 0; i + 1 < vl; i++)
        if (v[i] >= '0' && v[i] <= '9') { int w = 0, j = i; while (j < vl && v[j] >= '0' && v[j] <= '9') { w = w*10 + (v[j]-'0'); j++; } if (j < vl && v[j] == 'p') { width = w; break; } }
    width = width < 1 ? 1 : (width > 15 ? 15 : width);          /* 4-bit width */
    uint32_t color = 0x666666; int found = 0;                   /* default medium grey */
    for (int i = 0; i < vl && !found; ) {                        /* scan each token for a colour (#hex, rgb(), or a name like "red") */
        while (i < vl && (v[i]==' '||v[i]=='\t')) i++;
        int ts = i; while (i < vl && v[i]!=' ' && v[i]!='\t') i++;
        if (i <= ts) continue;
        uint32_t pc = parse_color(v + ts, i - ts);              /* parse_color reads the leading token of what we pass */
        if (pc != 0) { color = pc; found = 1; }                 /* any non-black colour token wins (width/style words parse to 0) */
        else if (v[ts]=='#' || (i-ts==5 && v[ts]=='b'&&v[ts+1]=='l'&&v[ts+2]=='a'&&v[ts+3]=='c'&&v[ts+4]=='k')) { color = 0; found = 1; }   /* explicit black / #000 */
    }
    return ((uint32_t)(width & 0xF) << 28) | ((uint32_t)(sides & 0xF) << 24) | (color & 0xFFFFFFu);
}
#endif
