/* Host unit + fuzz test for the browser's Markdown->HTML and CSV->HTML
 * converters (kernel/mdconv.h). Those render UNTRUSTED local .md/.csv files
 * into HTML for parse_html, so the two things that matter are: (1) they never
 * read outside the input or write outside `cap` (ASan/UBSan verify), and
 * (2) they escape HTML metachars — including " inside the href/src/alt
 * attributes they emit (the M1755 fix). Build with ASan+UBSan; exit 0 = pass. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../kernel/mdconv.h"

static int fails = 0;
#define CK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } } while (0)

/* Convert `md` (NUL-terminated) into a fresh, exactly-`cap`-sized buffer and
 * return it NUL-terminated in a caller-managed static (cap+1 alloc so we can
 * terminate for strstr without the converter itself ever touching out[cap]). */
static char *conv_md(const char *md, char *out, int cap) {
    int p = md_to_html(md, (int)strlen(md), out, cap);
    if (p < 0 || p > cap) { fprintf(stderr, "FAIL: md p=%d out of [0,%d]\n", p, cap); fails++; p = 0; }
    out[p] = 0;
    return out;
}
static char *conv_csv(const char *csv, char *out, int cap) {
    int p = csv_to_html(csv, (int)strlen(csv), out, cap);
    if (p < 0 || p > cap) { fprintf(stderr, "FAIL: csv p=%d out of [0,%d]\n", p, cap); fails++; p = 0; }
    out[p] = 0;
    return out;
}
#define HAS(needle, msg) CK(strstr(out, needle) != NULL, msg)
#define HASNT(needle, msg) CK(strstr(out, needle) == NULL, msg)

static void regression(void) {
    char buf[65536];
    char *out;

    /* --- block structure --- */
    out = conv_md("# Title", buf, sizeof buf - 1);
    HAS("<h1>Title</h1>", "h1 heading");
    out = conv_md("### Sub", buf, sizeof buf - 1);
    HAS("<h3>Sub</h3>", "h3 heading");
    out = conv_md("###### Deep", buf, sizeof buf - 1);
    HAS("<h6>Deep</h6>", "h6 heading (clamped at 6)");
    out = conv_md("plain text here", buf, sizeof buf - 1);
    HAS("<p>plain text here</p>", "paragraph wrap");
    out = conv_md("line one\nline two", buf, sizeof buf - 1);
    HAS("<p>line one line two</p>", "soft-wrapped paragraph joins with a space");
    out = conv_md("a\n\nb", buf, sizeof buf - 1);
    HAS("<p>a</p>", "blank line closes paragraph");
    HAS("<p>b</p>", "second paragraph after blank");
    out = conv_md("> quoted", buf, sizeof buf - 1);
    HAS("<blockquote>quoted</blockquote>", "blockquote");
    out = conv_md("---", buf, sizeof buf - 1);
    HAS("<hr>", "horizontal rule ---");
    out = conv_md("***", buf, sizeof buf - 1);
    HAS("<hr>", "horizontal rule ***");

    /* --- lists --- */
    out = conv_md("- a\n- b", buf, sizeof buf - 1);
    HAS("<ul><li>a</li><li>b</li></ul>", "bullet list");
    out = conv_md("1. a\n2. b", buf, sizeof buf - 1);
    HAS("<ol><li>a</li><li>b</li></ol>", "ordered list");
    out = conv_md("- a\n1. b", buf, sizeof buf - 1);
    HAS("</ul><ol>", "switching ul->ol closes the ul first");

    /* --- fenced code: no inline processing inside --- */
    out = conv_md("```\n**not bold** <tag>\n```", buf, sizeof buf - 1);
    HAS("<pre>", "code fence opens pre");
    HAS("**not bold** &lt;tag&gt;", "fence content is escaped, not inline-parsed");
    HASNT("<b>", "no bold inside a fence");

    /* --- inline spans --- */
    out = conv_md("a **b** c", buf, sizeof buf - 1);
    HAS("a <b>b</b> c", "bold");
    out = conv_md("a *b* c", buf, sizeof buf - 1);
    HAS("a <i>b</i> c", "italic");
    out = conv_md("a ~~b~~ c", buf, sizeof buf - 1);
    HAS("a <s>b</s> c", "strikethrough");
    out = conv_md("say `x<y` now", buf, sizeof buf - 1);
    HAS("<code>x&lt;y</code>", "inline code escapes metachars");
    out = conv_md("open **bold", buf, sizeof buf - 1);
    HAS("<b>bold</b>", "unterminated bold auto-closes at line end");
    out = conv_md("5 \\* 3 = 15", buf, sizeof buf - 1);
    HAS("5 * 3 = 15", "backslash escapes the star (no italic)");
    HASNT("<i>", "escaped star did not open italic");

    /* --- links / images / autolinks --- */
    out = conv_md("[go](http://x/)", buf, sizeof buf - 1);
    HAS("<a href=\"http://x/\">go</a>", "link");
    out = conv_md("![cat](c.png)", buf, sizeof buf - 1);
    HAS("<img src=\"c.png\" alt=\"cat\">", "image");
    out = conv_md("see http://ex.com/p done", buf, sizeof buf - 1);
    HAS("<a href=\"http://ex.com/p\">http://ex.com/p</a>", "bare-URL autolink");

    /* --- THE M1755 FIX: a " inside link/img text must be escaped, not break the attr --- */
    out = conv_md("![a\"b](u.png)", buf, sizeof buf - 1);
    HAS("alt=\"a&quot;b\"", "quote in alt text is escaped (M1755)");
    HASNT("alt=\"a\"b\"", "raw quote does NOT break the alt attribute");
    out = conv_md("[t](p?x=\"y\")", buf, sizeof buf - 1);
    HAS("href=\"p?x=&quot;y&quot;\"", "quote in link URL is escaped (M1755)");
    out = conv_md("plain \" quote", buf, sizeof buf - 1);
    HAS("&quot;", "quote in body text also escaped (decodes back to \" via htmlentity)");
    out = conv_md("a & b < c > d", buf, sizeof buf - 1);
    HAS("a &amp; b &lt; c &gt; d", "the other metachars still escape");

    /* --- GFM table --- */
    out = conv_md("| A | B |\n|---|---|\n| 1 | 2 |", buf, sizeof buf - 1);
    HAS("<table>", "table opens");
    HAS("<th>A</th>", "table header cell");
    HAS("<td>1</td>", "table body cell");
    HAS("</table>", "table closes");
    /* a pipe line with NO separator row is just a paragraph, not a table */
    out = conv_md("a | b | c", buf, sizeof buf - 1);
    HASNT("<table>", "pipes without a separator row are not a table");

    /* --- CSV --- */
    out = conv_csv("name,age\nBob,30", buf, sizeof buf - 1);
    HAS("<th>name</th>", "csv header cell");
    HAS("<td>Bob</td>", "csv body cell");
    out = conv_csv("\"a,b\",c", buf, sizeof buf - 1);
    HAS("<th>a,b</th>", "csv quoted field keeps the comma");
    out = conv_csv("\"he said \"\"hi\"\"\"", buf, sizeof buf - 1);
    HAS("he said &quot;hi&quot;", "csv doubled-quote is a literal, escaped quote");
    out = conv_csv("x<y,a&b", buf, sizeof buf - 1);
    HAS("<th>x&lt;y</th>", "csv escapes metachars");
    HAS("<th>a&amp;b</th>", "csv escapes ampersand");

    if (!fails) printf("regression: ok (blocks, inline, links, tables, CSV, quote-escape)\n");
}

/* --- fuzz: never read past the input, never write past cap (ASan enforces) --- */
static uint64_t rng = 0x1234567890abcdefULL;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }

/* Bias bytes toward markdown/CSV metachars so the fuzzer actually reaches the
 * link/image/table/quote branches, not just the plain-text fallthrough. */
static char rand_byte(void) {
    static const char meta[] = "#*_~`[]()!|>-+.:\"\\\n\r 0123456789abc";
    if (rnd() & 1) return meta[rnd() % (sizeof meta - 1)];
    return (char)(rnd() & 0xFF);
}

static void fuzz(void) {
    /* Exactly-cap-sized output buffers so ASan flags ANY write at index >= cap.
     * The converters guard every store with `*p < cap`, so a correct run never
     * touches out[cap]; a regression would trip the sanitizer here. */
    const int MAXIN = 4096;
    char *in = malloc(MAXIN);
    for (int iter = 0; iter < 300000; iter++) {
        int len = (int)(rnd() % (MAXIN + 1));
        for (int k = 0; k < len; k++) in[k] = rand_byte();
        int cap = 1 + (int)(rnd() % 8192);        /* incl. tiny caps that force truncation */
        char *out = malloc(cap);
        int which = rnd() & 1;
        int p = which ? md_to_html(in, len, out, cap) : csv_to_html(in, len, out, cap);
        if (p < 0 || p > cap) { fprintf(stderr, "FAIL: fuzz %s p=%d cap=%d len=%d\n",
                                        which ? "md" : "csv", p, cap, len); fails++; free(out); break; }
        free(out);
    }
    free(in);

    /* Truncation sweep: every prefix of a rich document must stay in-bounds
     * (an unterminated span/fence/table mid-cut is the classic OOB trigger). */
    const char *doc =
        "# H\n\ntext **b** *i* `c<d` [l](u\"v) ![a\"b](p)\n\n"
        "> q\n- x\n- y\n1. n\n\n```\ncode <t>\n```\n\n"
        "| A | B |\n|:-:|---|\n| 1 | 2 |\n| 3 | 4 |\nhttp://ex/\n";
    int dl = (int)strlen(doc);
    for (int cut = 0; cut <= dl; cut++) {
        char out[8192];
        int p = md_to_html(doc, cut, out, (int)sizeof out);
        if (p < 0 || p > (int)sizeof out) { fprintf(stderr, "FAIL: trunc cut=%d p=%d\n", cut, p); fails++; break; }
    }
    if (!fails) printf("fuzz: 300000 random md/csv (metachar-biased) + prefix truncations -> all in-bounds, ASan/UBSan clean\n");
}

int main(void) {
    regression();
    fuzz();
    if (fails) { fprintf(stderr, "FAIL: %d md/csv converter check(s) failed\n", fails); return 1; }
    printf("PASS: md/csv converters (blocks+inline+tables+quote-escape, ASan/UBSan fuzz clean)\n");
    return 0;
}
