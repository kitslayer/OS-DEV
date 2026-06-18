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
    free(s);
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
    expect("input[type]",1, "input", "", "", "type");
    expect("[CHECKED]",  1, "", "", "", "checked");      /* attr lowercased */
    expect("a[href=x]",  1, "a", "", "", "href");        /* =value ignored */
    expect("abcdefghijklmno", 1, "abcdefghijklmno", "", "", "");   /* 15-char tag: exact cap, still matches */
    expect("",           0, "", "", "", "");             /* empty -> no match */
    expect(">",          0, "", "", "", "");             /* child combinator unsupported -> fail closed */
    expect("div p",      0, "", "", "", "");             /* descendant combinator (space) -> fail closed */
    expect("div>p",      0, "", "", "", "");             /* `>` after tag -> fail closed */
    expect("a:hover",    0, "", "", "", "");             /* pseudo-class unsupported -> fail closed */
    expect("abcdefghijklmnopqrstuvwxyz", 0, "", "", "", "");   /* >15-char tag: overflow remainder hits else -> fail closed */
    printf("regression: %s\n", fails ? "FAILURES" : "ok (tag/class/id/attr + lowercase + =value-ignored + fail-closed + truncation)");

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
        if (fails) { printf("  (fuzz stopped at iter %d)\n", it); break; }
    }

    if (fails) { printf("FAIL: %d cssel.h check(s) failed\n", fails); return 1; }
    printf("PASS: cssel.h sel_parse (selector parse contract + bounds over 400k malformed selectors, ASan/UBSan clean)\n");
    return 0;
}
