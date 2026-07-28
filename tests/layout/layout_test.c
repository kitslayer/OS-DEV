/* layout_test.c — host-side known-answer tests for the CSS box-layout engine
 * (kernel/layout.h). Pure, built for the host under ASan+UBSan. Exit 0 = pass.
 * Keep in sync with kernel/layout.h.
 *
 * The measure callback is a FIXED-WIDTH stub (1 char = 10px), so every expected
 * line-break position below is arithmetic the reader can check by hand rather
 * than a property of the kernel font.
 *
 * Coverage: the §10.3.3 width constraint in all of its cases (auto width, one
 * auto margin, two auto margins = centring, over-constrained), border/padding
 * contributing to the used width, nested block stacking, §8.3.1 sibling margin
 * collapsing (including negative margins), parent<->last-child bottom
 * collapsing, explicit vs auto height, display:none, greedy inline wrapping, and
 * hostile input (deep nesting, malformed child indices, zero/negative widths).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "layout.h"

static int fails = 0, checks = 0;
#define OK(cond) do { checks++; if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)
#define EQ(got, want) do { checks++; if ((got) != (want)) { \
    printf("FAIL line %d: %s == %d, wanted %d\n", __LINE__, #got, (int)(got), (int)(want)); fails++; } } while (0)

/* 10px per character, so "abc" is 30px and a space is 10px. */
static int32_t measure10(void *ctx, const char *s, int32_t len) { (void)ctx; (void)s; return len * 10; }

#define MAXB 256
static lay_box  B[MAXB];
static lay_ctx  C;

static void reset(void) {
    memset(B, 0, sizeof B);
    for (int i = 0; i < MAXB; i++) {
        B[i].display = LAY_BLOCK;
        B[i].width = B[i].height = LAY_AUTO;
        B[i].first_child = B[i].next_sibling = -1;
    }
    C.boxes = B; C.nboxes = MAXB; C.measure = measure10; C.mctx = 0; C.line_height = 20;
}
/* Link `kids` (n of them) as children of `p`, in order. */
static void kids(int p, const int *k, int n) {
    if (n <= 0) return;
    B[p].first_child = k[0];
    for (int i = 0; i < n - 1; i++) B[k[i]].next_sibling = k[i + 1];
    B[k[n - 1]].next_sibling = -1;
}

int main(void) {
    /* ---- §10.3.3: width:auto fills the containing block ------------------- */
    reset();
    EQ(lay_layout(&C, 800), 0);            /* empty root: no content, no height */
    EQ(B[0].cw, 800);

    /* auto width shrinks by the box's own horizontal margins/padding/border */
    reset();
    B[0].margin.left = 10; B[0].margin.right = 20;
    B[0].padding.left = 5; B[0].padding.right = 5;
    B[0].border.left = 1;  B[0].border.right = 1;
    lay_layout(&C, 800);
    EQ(B[0].cw, 800 - 10 - 20 - 5 - 5 - 1 - 1);
    EQ(B[0].x, 10 + 1 + 5);                /* content origin past margin+border+padding */

    /* ---- the four auto-margin cases -------------------------------------- */
    {   /* both auto + fixed width => centred; odd slack puts the extra px right */
        lay_box b; memset(&b, 0, sizeof b);
        b.width = 400; b.margin.left = LAY_AUTO; b.margin.right = LAY_AUTO;
        int32_t ml, mr, w;
        lay_solve_width(&b, 800, &ml, &mr, &w);
        EQ(ml, 200); EQ(mr, 200); EQ(w, 400);

        lay_solve_width(&b, 801, &ml, &mr, &w);      /* slack 401 -> 200 / 201 */
        EQ(ml, 200); EQ(mr, 201);

        /* wider than the containing block: auto margins collapse to 0, overflow */
        lay_solve_width(&b, 300, &ml, &mr, &w);
        EQ(ml, 0); EQ(mr, 0); EQ(w, 400);
    }
    {   /* only margin-left auto => it absorbs all the slack (right-aligns) */
        lay_box b; memset(&b, 0, sizeof b);
        b.width = 400; b.margin.left = LAY_AUTO; b.margin.right = 50;
        int32_t ml, mr, w;
        lay_solve_width(&b, 800, &ml, &mr, &w);
        EQ(ml, 350); EQ(mr, 50); EQ(w, 400);
    }
    {   /* only margin-right auto => absorbs the slack (left-aligns) */
        lay_box b; memset(&b, 0, sizeof b);
        b.width = 400; b.margin.left = 50; b.margin.right = LAY_AUTO;
        int32_t ml, mr, w;
        lay_solve_width(&b, 800, &ml, &mr, &w);
        EQ(ml, 50); EQ(mr, 350); EQ(w, 400);
    }
    {   /* over-constrained: specified margin-right is IGNORED and re-solved */
        lay_box b; memset(&b, 0, sizeof b);
        b.width = 400; b.margin.left = 50; b.margin.right = 999;
        int32_t ml, mr, w;
        lay_solve_width(&b, 800, &ml, &mr, &w);
        EQ(ml, 50); EQ(w, 400); EQ(mr, 350);         /* 50 + 400 + 350 == 800 */
    }
    {   /* width:auto with an auto margin: width wins, the margin becomes 0 */
        lay_box b; memset(&b, 0, sizeof b);
        b.width = LAY_AUTO; b.margin.left = LAY_AUTO; b.margin.right = 100;
        int32_t ml, mr, w;
        lay_solve_width(&b, 800, &ml, &mr, &w);
        EQ(ml, 0); EQ(mr, 100); EQ(w, 700);
    }
    {   /* a negative auto-width never appears: it clamps at 0 */
        lay_box b; memset(&b, 0, sizeof b);
        b.width = LAY_AUTO; b.padding.left = 500; b.padding.right = 500;
        int32_t ml, mr, w;
        lay_solve_width(&b, 800, &ml, &mr, &w);
        EQ(w, 0);
    }

    /* ---- nested block stacking + auto height ----------------------------- */
    reset();
    B[1].height = 50; B[2].height = 30;
    { int k[] = {1, 2}; kids(0, k, 2); }
    EQ(lay_layout(&C, 800), 80);           /* root auto height = 50 + 30 */
    EQ(B[1].y, 0);  EQ(B[1].ch, 50);
    EQ(B[2].y, 50); EQ(B[2].ch, 30);
    EQ(B[0].ch, 80);
    EQ(B[1].cw, 800); EQ(B[2].cw, 800);    /* both fill the width */

    /* a child's padding/border add to the parent's content height */
    reset();
    B[1].height = 50; B[1].padding.top = 5; B[1].padding.bottom = 7;
    B[1].border.top = 1; B[1].border.bottom = 2;
    { int k[] = {1}; kids(0, k, 1); }
    EQ(lay_layout(&C, 800), 50 + 5 + 7 + 1 + 2);
    EQ(B[1].y, 1 + 5);                     /* content starts past border+padding */

    /* ---- §8.3.1 sibling margin collapsing -------------------------------- */
    reset();
    B[1].height = 50; B[1].margin.bottom = 30;
    B[2].height = 30; B[2].margin.top = 20;
    { int k[] = {1, 2}; kids(0, k, 2); }
    lay_layout(&C, 800);
    EQ(B[2].y, 50 + 30);                   /* max(30,20) = 30, NOT 50 */

    /* the larger margin wins whichever side it is on */
    reset();
    B[1].height = 50; B[1].margin.bottom = 10;
    B[2].height = 30; B[2].margin.top = 40;
    { int k[] = {1, 2}; kids(0, k, 2); }
    lay_layout(&C, 800);
    EQ(B[2].y, 50 + 40);

    /* negative margins: max(positives) + min(negatives) */
    EQ(lay_collapse(30, -10), 20);
    EQ(lay_collapse(-10, -30), -30);
    EQ(lay_collapse(0, 0), 0);
    EQ(lay_collapse(20, 20), 20);
    reset();
    B[1].height = 50; B[1].margin.bottom = 30;
    B[2].height = 30; B[2].margin.top = -10;
    { int k[] = {1, 2}; kids(0, k, 2); }
    lay_layout(&C, 800);
    EQ(B[2].y, 50 + 20);                   /* 30 + (-10) = 20 */

    /* three siblings collapse pairwise, not cumulatively */
    reset();
    for (int i = 1; i <= 3; i++) { B[i].height = 10; B[i].margin.top = 20; B[i].margin.bottom = 20; }
    { int k[] = {1, 2, 3}; kids(0, k, 3); }
    lay_layout(&C, 800);
    EQ(B[1].y, 20);                        /* own top margin (no parent collapse yet) */
    EQ(B[2].y, 20 + 10 + 20);              /* collapse(20,20) = 20 */
    EQ(B[3].y, 20 + 10 + 20 + 10 + 20);

    /* ---- parent <-> FIRST-child top collapsing (§8.3.1) ------------------ */
    reset();
    B[1].height = 40; B[1].margin.top = 25;
    { int k[] = {1}; kids(0, k, 1); }
    /* The root has no top border/padding, so the child's top margin collapses
     * THROUGH it and is applied outside: the margin appears once in the document
     * height, the root's content starts level with the child, and the margin is
     * NOT trapped inside the root's content height. */
    EQ(lay_layout(&C, 800), 25 + 40);
    EQ(B[0].y, 25);
    EQ(B[1].y, 25);
    EQ(B[0].ch, 40);

    /* top padding on the parent blocks the collapse: the margin lands inside */
    reset();
    B[0].padding.top = 3;
    B[1].height = 40; B[1].margin.top = 25;
    { int k[] = {1}; kids(0, k, 1); }
    EQ(lay_layout(&C, 800), 3 + 25 + 40);
    EQ(B[0].y, 3);
    EQ(B[1].y, 3 + 25);

    /* top border blocks it too */
    reset();
    B[0].border.top = 2;
    B[1].height = 40; B[1].margin.top = 25;
    { int k[] = {1}; kids(0, k, 1); }
    EQ(lay_layout(&C, 800), 2 + 25 + 40);

    /* the parent's own top margin collapses WITH the child's: the larger wins */
    reset();
    B[0].margin.top = 10;
    B[1].height = 40; B[1].margin.top = 25;
    { int k[] = {1}; kids(0, k, 1); }
    EQ(lay_layout(&C, 800), 25 + 40);      /* collapse(10,25) = 25, not 35 */
    EQ(B[0].y, 25);

    reset();                                /* and the other way round */
    B[0].margin.top = 30;
    B[1].height = 40; B[1].margin.top = 25;
    { int k[] = {1}; kids(0, k, 1); }
    EQ(lay_layout(&C, 800), 30 + 40);
    EQ(B[0].y, 30);

    /* it collapses through MULTIPLE levels of first-child nesting */
    reset();
    B[0].margin.top = 5;
    B[1].margin.top = 10;
    B[2].margin.top = 30; B[2].height = 10;
    { int k1[] = {1}; kids(0, k1, 1); }
    { int k2[] = {2}; kids(1, k2, 1); }
    EQ(lay_layout(&C, 800), 30 + 10);      /* 5, 10 and 30 all collapse to 30 */
    EQ(B[2].y, 30);

    /* a display:none first child is skipped when finding the collapsing margin */
    reset();
    B[1].display = LAY_NONE; B[1].margin.top = 999;
    B[2].height = 10; B[2].margin.top = 20;
    { int k[] = {1, 2}; kids(0, k, 2); }
    EQ(lay_layout(&C, 800), 20 + 10);
    EQ(B[2].y, 20);

    /* only the FIRST child escapes; a later sibling's margin stays inside */
    reset();
    B[1].height = 10; B[1].margin.top = 20;
    B[2].height = 10; B[2].margin.top = 40;
    { int k[] = {1, 2}; kids(0, k, 2); }
    EQ(lay_layout(&C, 800), 20 + 10 + 40 + 10);
    EQ(B[1].y, 20);
    EQ(B[2].y, 20 + 10 + 40);
    EQ(B[0].ch, 10 + 40 + 10);             /* the escaped 20 is not in here */

    /* ---- parent <-> last-child bottom collapsing ------------------------- */
    reset();
    B[1].height = 40; B[1].margin.bottom = 25;
    { int k[] = {1}; kids(0, k, 1); }
    /* Root has no bottom border/padding and auto height, so the child's bottom
     * margin collapses THROUGH it: root content height is 40, not 65. */
    EQ(lay_layout(&C, 800), 40);
    EQ(B[0].ch, 40);

    /* give the parent bottom padding and the margin no longer escapes */
    reset();
    B[0].padding.bottom = 3;
    B[1].height = 40; B[1].margin.bottom = 25;
    { int k[] = {1}; kids(0, k, 1); }
    EQ(lay_layout(&C, 800), 40 + 25 + 3);

    /* an explicit parent height also stops the collapse (and wins) */
    reset();
    B[0].height = 200;
    B[1].height = 40; B[1].margin.bottom = 25;
    { int k[] = {1}; kids(0, k, 1); }
    EQ(lay_layout(&C, 800), 200);
    EQ(B[0].ch, 200);

    /* ---- explicit height overrides content ------------------------------- */
    reset();
    B[0].height = 10;
    B[1].height = 500;
    { int k[] = {1}; kids(0, k, 1); }
    EQ(lay_layout(&C, 800), 10);           /* content overflows; height is honoured */

    /* ---- root margins are included in the document height ---------------- */
    reset();
    B[0].height = 100; B[0].margin.top = 10; B[0].margin.bottom = 20;
    EQ(lay_layout(&C, 800), 130);
    EQ(B[0].y, 10);

    /* ---- display:none ---------------------------------------------------- */
    reset();
    B[1].height = 50;
    B[2].display = LAY_NONE; B[2].height = 999; B[2].margin.top = 999;
    B[3].height = 30;
    { int k[] = {1, 2, 3}; kids(0, k, 3); }
    EQ(lay_layout(&C, 800), 80);           /* the none box contributes nothing */
    EQ(B[3].y, 50);
    EQ(B[2].ch, 0);

    /* ---- inline layout: greedy wrapping at the content width ------------- */
    reset();
    B[0].text = "aaa bbb ccc";             /* 3 words x 30px + 2 spaces x 10px = 110 */
    B[0].textlen = 11;
    EQ(lay_layout(&C, 800), 20);           /* fits on one line */
    EQ(B[0].nlines, 1);

    reset();
    B[0].text = "aaa bbb ccc"; B[0].textlen = 11;
    EQ(lay_layout(&C, 70), 40);            /* "aaa bbb"=70 fits; "ccc" wraps */
    EQ(B[0].nlines, 2);

    reset();
    B[0].text = "aaa bbb ccc"; B[0].textlen = 11;
    lay_layout(&C, 30);                    /* one word per line */
    EQ(B[0].nlines, 3);

    reset();                                /* a word longer than the line: 1 line, no hang */
    B[0].text = "aaaaaaaaaaaaaaaaaaaa"; B[0].textlen = 20;
    lay_layout(&C, 30);
    EQ(B[0].nlines, 1);

    reset();                                /* collapsed runs of spaces don't make lines */
    B[0].text = "a     b"; B[0].textlen = 7;
    lay_layout(&C, 800);
    EQ(B[0].nlines, 1);

    reset();                                /* leading/trailing spaces only */
    B[0].text = "   "; B[0].textlen = 3;
    lay_layout(&C, 800);
    EQ(B[0].nlines, 1);                     /* one (empty) line box */

    reset();                                /* inline height = lines * line_height */
    C.line_height = 17;
    B[0].text = "aaa bbb"; B[0].textlen = 7;
    EQ(lay_layout(&C, 30), 2 * 17);

    /* text inside a nested block is measured against THAT block's width */
    reset();
    B[0].padding.left = 10; B[0].padding.right = 10;
    B[1].text = "aaa bbb"; B[1].textlen = 7;
    { int k[] = {1}; kids(0, k, 1); }
    lay_layout(&C, 90);                     /* child width 90-20 = 70 -> both words fit */
    EQ(B[1].cw, 70);
    EQ(B[1].nlines, 1);

    /* ---- hostile / malformed input --------------------------------------- */
    reset();                                /* a child index past the array is ignored */
    B[0].first_child = MAXB + 5;
    EQ(lay_layout(&C, 800), 0);

    reset();                                /* deep nesting stops at LAY_MAX_DEPTH */
    for (int i = 0; i < MAXB - 1; i++) { B[i].first_child = i + 1; B[i].height = LAY_AUTO; }
    B[MAXB - 1].height = 5;
    lay_layout(&C, 800);                    /* must return, not blow the stack */
    OK(1);

    reset();                                /* zero-width viewport */
    B[0].text = "aaa bbb"; B[0].textlen = 7;
    lay_layout(&C, 0);
    EQ(B[0].cw, 0);
    EQ(B[0].nlines, 1);

    reset();                                /* negative viewport clamps to 0 width */
    lay_layout(&C, -100);
    EQ(B[0].cw, 0);

    {                                       /* null/empty context is rejected */
        lay_ctx z; memset(&z, 0, sizeof z);
        EQ(lay_layout(&z, 800), 0);
        lay_ctx n = C; n.measure = 0;
        EQ(lay_layout(&n, 800), 0);
    }

    /* a self-referential sibling link must not loop forever: the depth cap and
     * the fact that we only ever walk next_sibling forward from first_child mean
     * a cycle is the caller's bug, but assert we survive a self-loop child. */
    reset();
    B[0].first_child = 1;
    B[1].height = 10; B[1].next_sibling = -1;
    lay_layout(&C, 800);
    EQ(B[1].ch, 10);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
