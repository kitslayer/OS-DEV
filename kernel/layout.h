/* layout.h — a real CSS box-layout engine: block + inline formatting contexts.
 *
 * --- Why this exists ---------------------------------------------------------
 * kernel/browser.c renders by emitting a FLAT TOKEN STREAM (tok_t: an offset,
 * a length, a style, and a handful of per-token attributes like colour and
 * indent). There is no box tree, so there is nothing that can compute a
 * containing block, resolve `width:auto`, collapse margins, or centre a block
 * with `margin:0 auto` — `display:flex` is faked by bracketing tokens so that
 * block-breaks become horizontal gaps. GOALS.md names a real layout engine as
 * the next big build, and this is it.
 *
 * The engine is deliberately SEPARATE from browser.c and completely pure, for
 * two reasons: it can be exercised off-target with known-answer tests (see
 * tests/layout), and landing it carries zero regression risk for the existing
 * renderer until it is wired in.
 *
 * --- What it implements (CSS 2.1 §8 and §10) ---------------------------------
 * Block formatting context:
 *   - §10.3.3 the width constraint: for a non-replaced block in normal flow,
 *     margin-left + border-left + padding-left + width + padding-right +
 *     border-right + margin-right == containing-block width. `auto` values are
 *     solved in the order the spec requires: over-constrained boxes ignore
 *     margin-right; one `auto` margin absorbs the slack; two `auto` margins
 *     split it (which is what centres a block).
 *   - §10.6.3 auto height: the content height runs from the top content edge to
 *     the bottom margin edge of the last in-flow child.
 *   - §8.3.1 margin collapsing, in the two cases implemented here: adjacent
 *     SIBLINGS collapse to the larger margin, and a parent with no bottom
 *     border/padding and `height:auto` lets its last child's bottom margin
 *     collapse THROUGH it. Positive and negative margins combine as
 *     max(positives) + min(negatives). The parent <-> FIRST-child top-margin
 *     collapse is implemented too, via the lay_margin_top() lookahead: applying
 *     it inline would be circular (a child's position depends on the collapsed
 *     margin, and whether it collapses depends on the child), so the first-child
 *     chain is walked up front to find the margin that escapes upward.
 * Inline formatting context:
 *   - text is broken into line boxes at the content width, measured through a
 *     caller-supplied callback so the engine never depends on a font.
 *
 * --- Conventions -------------------------------------------------------------
 * Integer pixels throughout (no FPU in the kernel). The caller owns the box
 * array; the engine allocates nothing and never recurses deeper than the tree
 * (depth is capped, so a hostile document cannot blow the stack). All geometry
 * outputs describe the CONTENT box: (x, y, cw, ch). The margin-box height a
 * parent needs for stacking is reported separately in `mbh`.
 */
#ifndef LAYOUT_H
#define LAYOUT_H
#include <stdint.h>
#include <stddef.h>

#define LAY_AUTO      INT32_MIN     /* "this length is `auto`" */
#define LAY_MAX_DEPTH 64            /* hostile-nesting guard */

/* display types we lay out */
enum { LAY_BLOCK = 0, LAY_INLINE, LAY_INLINE_BLOCK, LAY_NONE };

typedef struct { int32_t top, right, bottom, left; } lay_edge;

typedef struct {
    uint8_t  display;
    int32_t  width, height;      /* LAY_AUTO or a used px length            */
    lay_edge margin, padding, border;

    /* Inline content (LAY_INLINE / LAY_INLINE_BLOCK). `text` is not owned. */
    const char *text;
    int32_t     textlen;

    /* Tree links: indices into the caller's box array, -1 for none. */
    int32_t first_child, next_sibling;

    /* --- results, filled by lay_layout ----------------------------------- */
    int32_t x, y;                /* content-box origin, document coordinates */
    int32_t cw, ch;              /* content-box width / height               */
    int32_t mbh;                 /* margin-box height (for the parent's flow) */
    int32_t nlines;              /* line boxes generated (inline content)     */
} lay_box;

/* Measure `len` bytes of text in px. The engine calls this and nothing else, so
 * it stays independent of the kernel font (the host test passes a fixed-width
 * stub). `ctx` is the caller's opaque cookie. */
typedef int32_t (*lay_measure_fn)(void *ctx, const char *s, int32_t len);

typedef struct {
    lay_box       *boxes;
    int32_t        nboxes;
    lay_measure_fn measure;
    void          *mctx;
    int32_t        line_height;   /* used height of one line box (px)         */
} lay_ctx;

/* --- small helpers ---------------------------------------------------------- */
static inline int32_t lay_max(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t lay_min(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t lay_def(int32_t v, int32_t d) { return v == LAY_AUTO ? d : v; }

/* Collapse two adjoining margins (CSS 2.1 §8.3.1): the larger of the positive
 * parts plus the smaller (most negative) of the negative parts. */
static inline int32_t lay_collapse(int32_t a, int32_t b) {
    int32_t pos = lay_max(a > 0 ? a : 0, b > 0 ? b : 0);
    int32_t neg = lay_min(a < 0 ? a : 0, b < 0 ? b : 0);
    return pos + neg;
}

/* Resolve the horizontal width constraint for a block-level box in normal flow
 * (§10.3.3) inside a containing block of width `avail`. Writes the used
 * margin-left/right and content width. Exposed separately because it is the
 * single trickiest rule in block layout and deserves its own tests. */
static inline void lay_solve_width(const lay_box *b, int32_t avail,
                                   int32_t *out_ml, int32_t *out_mr, int32_t *out_w) {
    int32_t bl = b->border.left + b->padding.left;
    int32_t br = b->border.right + b->padding.right;
    int32_t ml = b->margin.left, mr = b->margin.right, w = b->width;

    int ml_auto = (ml == LAY_AUTO), mr_auto = (mr == LAY_AUTO), w_auto = (w == LAY_AUTO);
    if (ml_auto) ml = 0;
    if (mr_auto) mr = 0;

    if (w_auto) {
        /* width:auto wins all the slack; auto margins then become 0. */
        w = avail - ml - mr - bl - br;
        if (w < 0) w = 0;
    } else {
        int32_t slack = avail - (ml + bl + w + br + mr);
        if (ml_auto && mr_auto)      {
            /* Centre the box. §10.3.3 says both used values are equal; with
             * integer pixels an odd slack cannot split evenly, so the extra pixel
             * goes on the RIGHT (ml = floor(slack/2)), which is what browsers do
             * for `margin: 0 auto`. A NEGATIVE slack means the box is wider than
             * its containing block: auto margins become 0 and it overflows to the
             * right rather than being pulled left by a negative margin. */
            if (slack <= 0) { ml = 0; mr = 0; }
            else            { ml = slack / 2; mr = slack - ml; }
        } else if (ml_auto)          { ml = slack; }
        else if (mr_auto)            { mr = slack; }
        else                         { mr += slack; }   /* over-constrained: §10.3.3
                                                         * ignores the specified
                                                         * margin-right (LTR) */
    }
    *out_ml = ml; *out_mr = mr; *out_w = w;
}

/* Lay out the inline text of `b` into line boxes at content width `cw`.
 * Greedy word wrapping on spaces, which is what the token renderer does today.
 * Returns the number of line boxes (>=1 whenever there is any text). */
static inline int32_t lay_inline_lines(lay_ctx *c, const lay_box *b, int32_t cw) {
    if (!b->text || b->textlen <= 0) return 0;
    if (cw <= 0) return 1;                    /* degenerate: everything on one line */

    int32_t lines = 1, used = 0, i = 0;
    while (i < b->textlen) {
        /* Skip a run of spaces; a break here is free. */
        while (i < b->textlen && b->text[i] == ' ') i++;
        if (i >= b->textlen) break;
        int32_t start = i;
        while (i < b->textlen && b->text[i] != ' ') i++;
        int32_t wlen = i - start;
        int32_t ww   = c->measure(c->mctx, b->text + start, wlen);
        int32_t sw   = (used > 0) ? c->measure(c->mctx, " ", 1) : 0;

        if (used > 0 && used + sw + ww > cw) {   /* doesn't fit: start a new line */
            lines++;
            used = ww;
        } else {
            used += sw + ww;
        }
        /* A single word longer than the line still occupies its own line; the
         * renderer clips it. Do NOT loop forever trying to fit it. */
    }
    return lines;
}

/* Forward declaration: block layout recurses. */
static inline int32_t lay_block(lay_ctx *c, int32_t idx, int32_t x, int32_t y,
                                int32_t avail, int depth);

/* The first in-flow (non-display:none) child of `idx`, or -1. */
static inline int32_t lay_first_inflow(lay_ctx *c, int32_t idx) {
    if (idx < 0 || idx >= c->nboxes) return -1;
    int32_t ci = c->boxes[idx].first_child;
    int guard = 0;
    while (ci >= 0 && ci < c->nboxes && c->boxes[ci].display == LAY_NONE
           && guard++ < c->nboxes)
        ci = c->boxes[ci].next_sibling;
    return (ci >= 0 && ci < c->nboxes) ? ci : -1;
}

/* The effective top margin of box `idx` AS ITS PARENT SEES IT: its own top
 * margin collapsed with every top margin escaping from its first in-flow
 * descendants (CSS 2.1 §8.3.1, the parent <-> first-child case).
 *
 * This is a pure LOOKAHEAD — it computes no positions — which is precisely what
 * makes the rule tractable. Applying it inline would be circular: a child's
 * position depends on the collapsed margin, and whether the margin collapses
 * depends on the child. Walking the first-child chain up front breaks that.
 *
 * Collapsing stops at the first box whose top border or padding separates its
 * own top edge from its first child's (that content edge is what blocks the
 * collapse), and the walk is depth-capped so a hostile document cannot spin. */
static inline int32_t lay_margin_top(lay_ctx *c, int32_t idx) {
    int32_t m = 0;
    for (int d = 0; d < LAY_MAX_DEPTH && idx >= 0 && idx < c->nboxes; d++) {
        lay_box *b = &c->boxes[idx];
        if (b->display == LAY_NONE) break;
        m = lay_collapse(m, b->margin.top == LAY_AUTO ? 0 : b->margin.top);
        if (b->border.top || b->padding.top) break;     /* blocks the collapse */
        int32_t ci = lay_first_inflow(c, idx);
        if (ci < 0) break;                              /* no child to collapse with */
        idx = ci;
    }
    return m;
}

/* Lay out the children of `parent` as a block formatting context starting at
 * content origin (cx, cy) with content width `cw`. Returns the content height
 * consumed, and reports the margin that is left dangling at the bottom (for the
 * parent's own bottom-margin collapsing) via *out_last_margin. */
static inline int32_t lay_block_children(lay_ctx *c, const lay_box *parent,
                                         int32_t cx, int32_t cy, int32_t cw,
                                         int depth, int skip_first_top,
                                         int32_t *out_last_margin) {
    int32_t yy = cy, prev_margin = 0, height = 0;
    int first = 1;
    *out_last_margin = 0;

    for (int32_t ci = parent->first_child; ci >= 0; ) {
        if (ci >= c->nboxes) break;                  /* malformed tree: stop */
        lay_box *ch = &c->boxes[ci];
        int32_t next = ch->next_sibling;
        if (ch->display == LAY_NONE) { ci = next; continue; }

        /* This child's effective top margin, collapsed with the margin still
         * pending from the previous sibling. `skip_first_top` means our own top
         * edge does not separate us from this first child, so the child's top
         * margin already ESCAPED upward (our caller applied it via
         * lay_margin_top) and must not be counted again here. */
        int32_t mt = (first && skip_first_top) ? 0 : lay_margin_top(c, ci);
        yy += lay_collapse(prev_margin, mt);
        first = 0;

        int32_t mbh = lay_block(c, ci, cx, yy, cw, depth + 1);
        yy += mbh;
        height = yy - cy;

        prev_margin  = (ch->margin.bottom == LAY_AUTO) ? 0 : ch->margin.bottom;
        ci = next;
    }
    *out_last_margin = prev_margin;
    return height;
}

/* Lay out box `idx` as a block-level box whose margin box starts at (x, y)
 * inside a containing block of width `avail`. Fills the box's geometry and
 * returns its margin-box height EXCLUDING the bottom margin still available for
 * collapsing (that is returned through the box's own mbh bookkeeping below). */
static inline int32_t lay_block(lay_ctx *c, int32_t idx, int32_t x, int32_t y,
                                int32_t avail, int depth) {
    if (idx < 0 || idx >= c->nboxes) return 0;
    lay_box *b = &c->boxes[idx];
    if (b->display == LAY_NONE) { b->x = b->y = b->cw = b->ch = b->mbh = 0; return 0; }
    if (depth >= LAY_MAX_DEPTH) {                   /* hostile nesting: stop flat */
        b->x = x; b->y = y; b->cw = 0; b->ch = 0; b->mbh = 0; return 0;
    }

    int32_t ml, mr, w;
    lay_solve_width(b, avail, &ml, &mr, &w);

    b->x  = x + ml + b->border.left + b->padding.left;
    b->y  = y + b->border.top + b->padding.top;     /* caller already applied margin-top */
    b->cw = w;

    /* --- content height ---------------------------------------------------- */
    int32_t content_h = 0;
    b->nlines = 0;
    if (b->first_child >= 0) {
        /* Block container with children: run a block formatting context. Bottom
         * margin collapsing (§8.3.1): if we have no bottom padding/border, the
         * last child's bottom margin collapses THROUGH us rather than adding. */
        int32_t last_margin = 0;
        /* Our own top edge blocks the first child's top-margin collapse only if
         * we have top border or padding; otherwise that margin escaped to our
         * caller via lay_margin_top and must not be applied again inside. */
        int skip_first_top = (b->border.top == 0 && b->padding.top == 0);
        content_h = lay_block_children(c, b, b->x, b->y, b->cw, depth,
                                       skip_first_top, &last_margin);
        /* lay_block_children stops at the last child's border edge: the trailing
         * bottom margin is deliberately NOT in content_h yet, because whether it
         * belongs to us depends on collapsing. If we have no bottom border or
         * padding (and no explicit height fixing our bottom edge) that margin
         * collapses THROUGH us and contributes nothing; otherwise it is trapped
         * inside and adds to our content height. Adding it in the non-collapsing
         * case is the correct direction — an earlier version SUBTRACTED it in the
         * collapsing case, which double-counted and made a 40px child inside an
         * auto-height parent report 15px. */
        if (!(b->border.bottom == 0 && b->padding.bottom == 0 && b->height == LAY_AUTO))
            content_h += last_margin;
        if (content_h < 0) content_h = 0;
    } else if (b->text && b->textlen > 0) {
        b->nlines = lay_inline_lines(c, b, b->cw);
        content_h  = b->nlines * c->line_height;
    }

    b->ch = (b->height == LAY_AUTO) ? content_h : b->height;

    /* Margin-box height for the parent's flow. The TOP margin was consumed by
     * the caller (collapsing), and the BOTTOM margin is left for the caller to
     * collapse with the next sibling, so neither is included here. */
    b->mbh = b->border.top + b->padding.top + b->ch
           + b->padding.bottom + b->border.bottom;
    return b->mbh;
}

/* Lay out the tree rooted at box 0 inside a viewport `vw` px wide, with its
 * content origin at (0,0). Returns the total document height (the root's
 * margin-box height including its own top/bottom margins). */
static inline int32_t lay_layout(lay_ctx *c, int32_t vw) {
    if (!c || !c->boxes || c->nboxes <= 0 || !c->measure) return 0;
    if (c->line_height <= 0) c->line_height = 1;
    lay_box *r = &c->boxes[0];
    /* The root's effective top margin includes anything collapsing out of its
     * first in-flow descendants (§8.3.1) — the reason `<body><p>text` does not
     * get double the intended leading. */
    int32_t mt = lay_margin_top(c, 0);
    int32_t mb = (r->margin.bottom == LAY_AUTO) ? 0 : r->margin.bottom;
    int32_t h  = lay_block(c, 0, 0, mt, vw, 0);
    return mt + h + mb;
}

#endif /* LAYOUT_H */
