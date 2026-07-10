/* Host regression + fuzz for kernel/include/cssel.h — sel_parse, the CSS simple-selector
 * parser the browser runs over untrusted <style>/inline selectors. It walks a NUL-
 * terminated selector string filling fixed fields (tag[16]/cls[32]/id[32]/attr[32]); a
 * long or adversarial selector must truncate/fail-closed, never OOB. We #include the real
 * header and drive it with EXACTLY-sized NUL-terminated inputs (malloc len+1) so any read
 * past the string red-zones under ASan, and we assert each output field stays NUL-
 * terminated within its size. Build under ASan+UBSan via tests/run-cssel-tests.sh.
 *
 * Oracle (verified): change the tag loop guard `k<15` to `k<16` and the
 * "tag overran" invariant below fires (tag[15] is then a data char, NUL lands at [16],
 * strlen(tag)==16 == sizeof(tag)); change a `while (s[i] && ...)` to drop the `s[i]`
 * NUL check and ASan trips on the read past the exactly-sized input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/include/cssel.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

static void expect(const char *sel, int ret, const char *tag, const char *cls, const char *id, const char *attr) {
    sel_t s;
    int r = sel_parse(sel, &s);
    char m[256];
    snprintf(m, sizeof(m), "sel_parse('%s') -> ret=%d tag='%s' cls='%s' id='%s' attr='%s'", sel, r, s.tag, s.cls, s.id, s.attr);
    CHECK(r == ret, m);
    if (ret) {
        CHECK(strcmp(s.tag, tag) == 0, m);
        CHECK(strcmp(s.cls, cls) == 0, m);
        CHECK(strcmp(s.id, id) == 0, m);
        CHECK(strcmp(s.attr, attr) == 0, m);
    }
}

/* M1793: [attr OP value] — verify the captured value (aval) and operator (aop). */
static void expect_av(const char *sel, const char *attr, const char *aval, char aop) {
    sel_t s;
    int r = sel_parse(sel, &s);
    char m[256];
    snprintf(m, sizeof(m), "sel_parse('%s') -> ret=%d attr='%s' aval='%s' aop='%c'", sel, r, s.attr, s.aval, s.aop ? s.aop : '0');
    CHECK(r == 1, m);
    CHECK(strcmp(s.attr, attr) == 0, m);
    CHECK(strcmp(s.aval, aval) == 0, m);
    CHECK(s.aop == aop, m);
}

/* descendant selector (M1434): target in tag/cls, nearest-ancestor requirement in dtag/dcls */
static void expect_desc(const char *sel, const char *tag, const char *cls, const char *dtag, const char *dcls) {
    sel_t s;
    int r = sel_parse(sel, &s);
    char m[256];
    snprintf(m, sizeof(m), "sel_parse('%s') -> ret=%d tag='%s' cls='%s' dtag='%s' dcls='%s'", sel, r, s.tag, s.cls, s.dtag, s.dcls);
    CHECK(r == 1, m);
    CHECK(strcmp(s.tag, tag) == 0, m);
    CHECK(strcmp(s.cls, cls) == 0, m);
    CHECK(strcmp(s.dtag, dtag) == 0, m);
    CHECK(strcmp(s.dcls, dcls) == 0, m);
}

/* exactly-sized, NUL-terminated input so any over-read past s[len] red-zones */
static void fuzz_one(const unsigned char *data, int len) {
    char *s = malloc((size_t)len + 1);
    if (len) memcpy(s, data, (size_t)len);
    s[len] = 0;
    sel_t o;
    sel_parse(s, &o);
    CHECK(strlen(o.tag)  < sizeof(o.tag),  "tag field overran");
    CHECK(strlen(o.cls)  < sizeof(o.cls),  "cls field overran");
    CHECK(strlen(o.id)   < sizeof(o.id),   "id field overran");
    CHECK(strlen(o.attr) < sizeof(o.attr), "attr field overran");
    CHECK(strlen(o.dtag) < sizeof(o.dtag), "dtag field overran");
    CHECK(strlen(o.dcls) < sizeof(o.dcls), "dcls field overran");
    free(s);
}

static void expect_cls(const char *v, const char *cls, int want) {
    int r = class_has(v, (int)strlen(v), cls);
    char m[160]; snprintf(m, sizeof(m), "class_has('%s','%s')=%d want=%d", v, cls, r, want);
    CHECK(r == want, m);
}
static void expect_cls_all(const char *v, const char *list, int want) {
    int r = class_has_all(v, (int)strlen(v), list);
    char m[160]; snprintf(m, sizeof(m), "class_has_all('%s','%s')=%d want=%d", v, list, r, want);
    CHECK(r == want, m);
}

/* class_has reads v[0..vl) (vl-bounded, never v[vl]) + the NUL-terminated cls; an
 * exactly-sized v (NO NUL) red-zones any over-read past the class-attribute slice. */
static void fuzz_cls(const unsigned char *data, int len, const char *cls) {
    char *v = malloc(len ? (size_t)len : 1);
    if (len) memcpy(v, data, (size_t)len);
    (void)class_has(v, len, cls);   /* must not OOB regardless of result */
    free(v);
}

int main(void) {
    /* --- regression: the matching contract the browser's css_match relies on --- */
    expect("div",        1, "div", "", "", "");
    expect("DIV",        1, "div", "", "", "");          /* tag lowercased */
    expect(".foo",       1, "", "foo", "", "");
    expect("#bar",       1, "", "", "bar", "");
    expect("[data-x]",   1, "", "", "", "data-x");
    expect("div.foo",    1, "div", "foo", "", "");
    expect("div.foo#bar",1, "div", "foo", "bar", "");
    expect("a.b#c[d]",   1, "a", "b", "c", "d");         /* all four components */
    expect("p.my-class", 1, "p", "my-class", "", "");    /* hyphen kept in class */
    expect("x.a_b",      1, "x", "a_b", "", "");         /* underscore kept */
    /* compound classes (M1775): every ".class" is retained, space-separated, not overwritten */
    expect(".btn.primary",   1, "",    "btn primary", "", "");   /* two classes both kept */
    expect("div.a.b",        1, "div", "a b",         "", "");   /* tag + two classes */
    expect(".x.y.z",         1, "",    "x y z",       "", "");   /* three classes */
    expect("li.item.active#f",1,"li",  "item active", "f", "");  /* classes accumulate, id still last */
    expect("input[type]",1, "input", "", "", "type");
    expect("[CHECKED]",  1, "", "", "", "checked");      /* attr lowercased */
    expect("a[href=x]",  1, "a", "", "", "href");        /* name "href" (value captured in aval — see expect_av below) */
    expect("abcdefghijklmno", 1, "abcdefghijklmno", "", "", "");   /* 15-char tag: exact cap, still matches */
    expect("",           0, "", "", "", "");             /* empty -> no match */
    expect(">",          0, "", "", "", "");             /* bare combinator with no target -> fail closed */
    /* descendant selectors (M1434): split on the last whitespace; target = rightmost simple
     * selector, nearest ancestor's class/tag -> dtag/dcls requirement */
    expect_desc("div p",          "p",  "",     "div", "");
    expect_desc(".nav-links a",   "a",  "",     "",    "nav-links");
    expect_desc(".a .b",          "",   "b",    "",    "a");
    expect_desc("ul.menu li.item","li", "item", "ul",  "menu");   /* nearest ancestor; class preferred at match */
    expect_desc("section  p",     "p",  "",     "section", "");   /* collapsed extra whitespace */
    /* child/sibling combinators now treated like descendant (M1439) */
    expect_desc("div>p",          "p",  "",     "div", "");       /* child combinator, no spaces */
    expect_desc("div > p",        "p",  "",     "div", "");       /* child combinator with spaces */
    expect_desc("div p:hover",    "p",  "",     "div", "");       /* pseudo-class stripped on target */
    expect("#x p",       0, "", "", "", "");             /* id-only ancestor unsupported -> fail closed */
    expect("abcdefghijklmnopqrstuvwxyz", 0, "", "", "", "");   /* >15-char tag: overflow remainder hits else -> fail closed */
    /* --- pseudo-class / pseudo-element stripping (M1439) --- */
    expect("a:hover",        1, "a",    "", "", "");    /* :hover stripped, tag "a" kept */
    expect("li:first-child", 1, "li",   "", "", "");    /* :first-child stripped */
    expect("p::before",      1, "p",    "", "", "");    /* ::before (pseudo-element) stripped */
    expect("a:not(.x)",      1, "a",    "", "", "");    /* :not(.x) stripped, tag "a" kept */
    expect(":root",          1, "html", "", "", "");    /* :root -> tag "html" */
    /* child / sibling combinators (M1439) */
    expect_desc(".menu > li",    "li", "",  "",    "menu");   /* child combinator with class ancestor */
    expect_desc("h2 + p",        "p",  "",  "h2",  "");       /* adjacent sibling */
    expect_desc("ul ~ p",        "p",  "",  "ul",  "");       /* general sibling */
    /* M1793: attribute-VALUE selectors — the value + operator are now CAPTURED (aval/aop),
     * no longer ignored. The attr NAME still parses as before (verified by expect() above). */
    expect("[data-x=\"y\"]",     1, "", "", "", "data-x");   /* name still "data-x"; value now in aval */
    expect_av("a[href=x]",        "href",   "x",     '=');
    expect_av("input[type=text]", "type",   "text",  '=');   /* the common form-control case */
    expect_av("[data-x=\"y\"]",   "data-x", "y",     '=');   /* double-quoted value */
    expect_av("[data-x='y']",     "data-x", "y",     '=');   /* single-quoted value */
    expect_av("[class~=box]",     "class",  "box",   '~');   /* whitespace-separated token */
    expect_av("[href^=https]",    "href",   "https", '^');   /* prefix */
    expect_av("[href$=pdf]",      "href",   "pdf",   '$');   /* suffix */
    expect_av("[title*=ell]",     "title",  "ell",   '*');   /* substring */
    expect_av("[data-v=Case]",    "data-v", "Case",  '=');   /* value is case-PRESERVED */
    expect_av("[type]",           "type",   "",      0);     /* presence-only: no op, empty aval */
    /* selector longer than 40 chars: must still parse (tests the 96-char cap in browser.c) */
    expect(".very-long-class-name-that-exceeds-forty a", 1, "a", "", "", "");
    /* class_has: a class matches only as a whole space/tab-separated token */
    expect_cls("foo", "foo", 1);
    expect_cls("a foo b", "foo", 1);
    expect_cls("foobar", "foo", 0);        /* prefix, not a whole token */
    expect_cls("barfoo", "foo", 0);        /* suffix */
    expect_cls("xfoo foox", "foo", 0);     /* partial at both ends */
    expect_cls("foo bar", "bar", 1);       /* token at end */
    expect_cls("foo\tbar", "bar", 1);      /* tab separator */
    expect_cls("  foo  ", "foo", 1);       /* surrounding whitespace */
    expect_cls("", "foo", 0);              /* empty class list */
    expect_cls("foo", "", 0);              /* empty target class */
    expect_cls("nav-link active", "active", 1);
    /* class_has_all: EVERY class in the compound list must be present as a whole token (M1775) */
    expect_cls_all("btn primary",        "btn primary", 1);   /* both present */
    expect_cls_all("btn",                "btn primary", 0);   /* missing "primary" */
    expect_cls_all("primary",            "btn primary", 0);   /* missing "btn" */
    expect_cls_all("a btn b primary c",  "btn primary", 1);   /* both present among others */
    expect_cls_all("btn primaryx",       "btn primary", 0);   /* "primaryx" is not the token "primary" */
    expect_cls_all("btn primary",        "btn",         1);   /* single-class list still works */
    expect_cls_all("anything",           "",            1);   /* empty list -> vacuously true */
    printf("regression: %s\n", fails ? "FAILURES" : "ok (tag/class/id/attr + [attr=val] value/op (M1793) + compound classes + lowercase + fail-closed + truncation + class_has word-boundary)");

    /* --- fuzz: random + structured selectors over exactly-sized inputs (ASan-checked) --- */
    unsigned int seed = 0x5e1ec701u;
    const char alpha[] = "abcXYZ0.#[]=-_: >*,\"'\t\\";
    for (int it = 0; it < 400000; it++) {
        seed = seed * 1103515245u + 12345u;
        int len = (int)(seed % 40);
        unsigned char buf[40];
        for (int i = 0; i < len; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = (seed & 0x100) ? (unsigned char)alpha[(seed >> 9) % (sizeof(alpha) - 1)]
                                    : (unsigned char)((seed >> 9) & 0xFF);   /* mix structured + raw bytes */
        }
        fuzz_one(buf, len);
        char cls[8]; int cn = (int)((seed >> 3) % 7);   /* a short random target class */
        for (int i = 0; i < cn; i++) { seed = seed * 1103515245u + 12345u; cls[i] = alpha[(seed >> 9) % (sizeof(alpha) - 1)]; }
        cls[cn] = 0;
        fuzz_cls(buf, len, cls);   /* class_has over the same exactly-sized slice + a random class */
        if (fails) { printf("  (fuzz stopped at iter %d)\n", it); break; }
    }

    if (fails) { printf("FAIL: %d cssel.h check(s) failed\n", fails); return 1; }
    printf("PASS: cssel.h sel_parse (selector parse contract + bounds over 400k malformed selectors, ASan/UBSan clean)\n");
    return 0;
}
