/* reader.c — reader-mode main-content extraction over untrusted page bytes.
 *
 * A token-stream HTML renderer lays elements out in source order, so modern pages
 * that put navigation / header / sidebar markup before the article bury the real
 * content under screens of chrome (open en.wikipedia.org and you scroll past the
 * whole sidebar before the article starts). reader_main_region() finds the page's
 * main-content container — semantic <main>/<article>, ARIA role="main", or a
 * well-known content id — and returns its inner byte range, so the renderer can
 * show the article first. Pure: every read is bounded by len; safe on hostile
 * bytes. Fuzzed in isolation by tests/reader.
 */
#include "reader.h"
#include "htmlattr.h"   /* lc(), find_attr() */

/* tag name at s[p..] equals `name` (length nl), terminated by a tag delimiter.
 * p is just past '<' (or '</'). Case-insensitive. Bounded by len. */
static int name_is(const char *s, int len, int p, const char *name, int nl) {
    if (p < 0 || p + nl > len) return 0;
    for (int k = 0; k < nl; k++)
        if (lc((unsigned char)s[p + k]) != name[k]) return 0;
    char e = (p + nl < len) ? s[p + nl] : '>';
    return e == '>' || e == ' ' || e == '\t' || e == '\n' || e == '\r' || e == '/';
}

/* advance from a '<' at p to just past the tag's '>' (quote-aware, so a '>' inside
 * an attribute value is not mistaken for the tag end). Returns len if unterminated. */
static int skip_tag(const char *s, int len, int p) {
    int j = p + 1; char q = 0;
    while (j < len) {
        char c = s[j];
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '>') { j++; break; }
        j++;
    }
    return j;
}

/* skip a raw-text element body (script/style): from te (just past the open tag's
 * '>') return the offset just past the matching close tag. `close` is the name
 * without the leading '/' (e.g. "script"). Bounded. */
static int skip_rawtext(const char *s, int len, int te, const char *close, int cl) {
    for (int k = te; k < len; k++) {
        if (s[k] == '<' && k + 1 < len && s[k + 1] == '/') {
            int ok = 1;
            for (int z = 0; z < cl; z++)
                if (k + 2 + z >= len || lc((unsigned char)s[k + 2 + z]) != close[z]) { ok = 0; break; }
            if (ok) return skip_tag(s, len, k);
        }
    }
    return len;
}

/* find the matching close tag </name> for an element whose inner content begins at
 * `start` (its opening tag's depth counted as 1). Returns the offset of the '<' of
 * the matching close tag, or -1 if unbalanced. Skips comments and script/style raw
 * text so a '<' inside them never miscounts the nesting. */
static int find_close(const char *s, int len, int start, const char *name, int nl) {
    int depth = 1, i = start;
    while (i < len) {
        if (s[i] != '<') { i++; continue; }
        if (i + 3 < len && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {  /* comment */
            int k = i + 4;
            while (k + 2 < len && !(s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>')) k++;
            i = (k + 2 < len) ? k + 3 : len;
            continue;
        }
        int closing = (i + 1 < len && s[i + 1] == '/');
        int np = i + 1 + (closing ? 1 : 0);
        if (!closing && name_is(s, len, np, "script", 6)) { i = skip_rawtext(s, len, skip_tag(s, len, i), "script", 6); continue; }
        if (!closing && name_is(s, len, np, "style", 5))  { i = skip_rawtext(s, len, skip_tag(s, len, i), "style", 5);  continue; }
        if (name_is(s, len, np, name, nl)) {
            if (closing) { depth--; if (depth == 0) return i; }
            else depth++;
        }
        i = skip_tag(s, len, i);
    }
    return -1;
}

/* visible (non-tag, non-whitespace) character count in s[a..b). A candidate region
 * must hold a real article's worth of text, so an empty/near-empty container (e.g.
 * a <main> that only frames a widget) is rejected and a better candidate tried. */
static int visible_len(const char *s, int a, int b) {
    int n = 0, intag = 0;
    for (int i = a; i < b && n < 1 << 20; i++) {
        char c = s[i];
        if (c == '<') { intag = 1; continue; }
        if (c == '>') { intag = 0; continue; }
        if (intag) continue;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        n++;
    }
    return n;
}

/* case-insensitive exact compare of an attribute value slice v[0..vl) to `lit`. */
static int val_is(const char *v, int vl, const char *lit) {
    int i = 0;
    for (; i < vl; i++) { if (!lit[i] || lc((unsigned char)v[i]) != lc((unsigned char)lit[i])) return 0; }
    return lit[i] == 0;
}

#define MIN_VIS 140   /* a region must hold this many visible chars to count as content */

/* Content-container ids. "Precise" ids name the article body itself and so beat
 * even the semantic <main>: e.g. MediaWiki's <main id=content> still wraps the
 * language bar, whereas #mw-content-text is just the article. "Generic" ids are
 * weaker wrapper names, tried only after the semantic <main>/role/<article>. */
static const char *const k_precise[] = { "mw-content-text", "bodyContent" };
static const char *const k_generic[] = { "content", "main-content", "main", "primary", "post", "entry-content" };
#define N_PRECISE ((int)(sizeof(k_precise) / sizeof(k_precise[0])))
#define N_GENERIC ((int)(sizeof(k_generic) / sizeof(k_generic[0])))

int reader_main_region(const char *body, int len, int *lo, int *hi) {
    if (!body || len <= 0) return 0;
    const char *s = body;

    int main_open = -1;                                  /* inner start of the first <main> */
    int role_open = -1; char role_tag[16] = {0}; int role_tl = 0;
    int mw_open   = -1; char mw_tag[16]   = {0}; int mw_tl   = 0;   /* a precise content id (#mw-content-text …) */
    int gen_open  = -1, gen_rank = N_GENERIC; char gen_tag[16] = {0}; int gen_tl = 0;   /* a generic wrapper id */
    int art_open = -1, art_count = 0;                    /* first <article> + how many */

    for (int i = 0; i < len; ) {
        if (s[i] != '<') { i++; continue; }
        if (i + 3 < len && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {  /* comment */
            int k = i + 4;
            while (k + 2 < len && !(s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>')) k++;
            i = (k + 2 < len) ? k + 3 : len;
            continue;
        }
        if (i + 1 < len && (s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?')) { i = skip_tag(s, len, i); continue; }

        int np = i + 1, p = np, tl = 0; char tag[16];
        while (p < len && tl < 15) {                     /* read the tag name */
            int c = lc((unsigned char)s[p]);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) break;
            tag[tl++] = (char)c; p++;
        }
        tag[tl] = 0;
        if (tl == 0) { i = skip_tag(s, len, i); continue; }

        if (tl == 6 && name_is(s, len, np, "script", 6)) { i = skip_rawtext(s, len, skip_tag(s, len, i), "script", 6); continue; }
        if (tl == 5 && name_is(s, len, np, "style", 5))  { i = skip_rawtext(s, len, skip_tag(s, len, i), "style", 5);  continue; }

        int te = skip_tag(s, len, i);                    /* just past '>' */
        int as = p, an = (te > 0 ? te - 1 : p) - p;      /* attribute slice [p, '>') */
        if (an < 0) an = 0;

        if (main_open < 0 && tl == 4 && tag[0] == 'm' && tag[1] == 'a' && tag[2] == 'i' && tag[3] == 'n')
            main_open = te;

        if (role_open < 0) {                             /* role="main" on any element */
            const char *rv; int rvl;
            if (find_attr(s + as, an, "role", &rv, &rvl) && val_is(rv, rvl, "main")) {
                role_open = te; role_tl = tl; for (int k = 0; k < tl; k++) role_tag[k] = tag[k]; role_tag[tl] = 0;
            }
        }
        {                                                /* id in the known-content sets */
            const char *iv; int ivl;
            if (find_attr(s + as, an, "id", &iv, &ivl)) {
                if (mw_open < 0)
                    for (int r = 0; r < N_PRECISE; r++)
                        if (val_is(iv, ivl, k_precise[r])) { mw_open = te; mw_tl = tl; for (int k = 0; k < tl; k++) mw_tag[k] = tag[k]; mw_tag[tl] = 0; break; }
                for (int r = 0; r < gen_rank; r++)
                    if (val_is(iv, ivl, k_generic[r])) { gen_rank = r; gen_open = te; gen_tl = tl; for (int k = 0; k < tl; k++) gen_tag[k] = tag[k]; gen_tag[tl] = 0; break; }
            }
        }

        if (tl == 7 && name_is(s, len, np, "article", 7)) { art_count++; if (art_open < 0) art_open = te; }

        i = te;
    }

    /* Try candidates tightest-first; accept the first whose region closes and holds
     * enough text. A precise content id (the article body) wins over the semantic
     * <main> (which often still wraps a language/title bar). A lone <article> is the
     * next-tightest; it's skipped on multi-<article> listings (a blog index). <main>
     * and role=main are the standard semantic fallbacks; generic ids are last. */
    int c;
    if (mw_open >= 0 && mw_tl > 0) {
        c = find_close(s, len, mw_open, mw_tag, mw_tl);
        if (c > mw_open && visible_len(s, mw_open, c) >= MIN_VIS) { *lo = mw_open; *hi = c; return 1; }
    }
    if (art_open >= 0 && art_count == 1) {
        c = find_close(s, len, art_open, "article", 7);
        if (c > art_open && visible_len(s, art_open, c) >= MIN_VIS) { *lo = art_open; *hi = c; return 1; }
    }
    if (main_open >= 0) {
        c = find_close(s, len, main_open, "main", 4);
        if (c > main_open && visible_len(s, main_open, c) >= MIN_VIS) { *lo = main_open; *hi = c; return 1; }
    }
    if (role_open >= 0 && role_tl > 0) {
        c = find_close(s, len, role_open, role_tag, role_tl);
        if (c > role_open && visible_len(s, role_open, c) >= MIN_VIS) { *lo = role_open; *hi = c; return 1; }
    }
    if (gen_open >= 0 && gen_tl > 0) {
        c = find_close(s, len, gen_open, gen_tag, gen_tl);
        if (c > gen_open && visible_len(s, gen_open, c) >= MIN_VIS) { *lo = gen_open; *hi = c; return 1; }
    }
    return 0;
}
