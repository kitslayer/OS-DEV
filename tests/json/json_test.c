/*
 * json_test.c — host unit tests for the JSON validator/pretty-printer
 * (user/jsoncore.h). Pure (no syscalls), so — exactly like tests/calc, tests/
 * sheet and tests/plot — we build it for the host under ASan/UBSan and check the
 * recursive-descent validate + pretty-print (structure, escapes, numbers, error
 * offsets, nesting). Exit 0 = all pass.
 */
#include <stdio.h>
#include <string.h>
#include "jsoncore.h"       /* -Iuser on the compile line */

static int fails, checks;

/* valid JSON `in` must reformat to exactly `want` (and report no error) */
static void ok(const char *in, const char *want) {
    checks++;
    char out[4096];
    int e = json_format(in, out, sizeof out);
    if (e >= 0) { printf("  FAIL ok(%s): unexpected error at %d\n", in, e); fails++; }
    else if (strcmp(out, want) != 0) { printf("  FAIL ok(%s):\n   got  \"%s\"\n   want \"%s\"\n", in, out, want); fails++; }
}
/* valid JSON `in` must MINIFY to exactly `want` (compact, no whitespace) */
static void mini(const char *in, const char *want) {
    checks++;
    char out[4096];
    int e = json_minify(in, out, sizeof out);
    if (e >= 0) { printf("  FAIL mini(%s): unexpected error at %d\n", in, e); fails++; }
    else if (strcmp(out, want) != 0) { printf("  FAIL mini(%s):\n   got  \"%s\"\n   want \"%s\"\n", in, out, want); fails++; }
}
/* invalid JSON `in` must report an error at byte offset `wantpos` */
static void bad(const char *in, int wantpos) {
    checks++;
    char out[4096];
    int e = json_format(in, out, sizeof out);
    if (e < 0) { printf("  FAIL bad(%s): expected an error, got success\n", in); fails++; }
    else if (e != wantpos) { printf("  FAIL bad(%s): error at %d, want %d\n", in, e, wantpos); fails++; }
}

int main(void) {
    printf("JSON validator/pretty-printer tests\n");

    /* scalars */
    ok("42", "42");
    ok("-3.14", "-3.14");
    ok("1.5e10", "1.5e10");
    ok("  true ", "true");
    ok("false", "false");
    ok("null", "null");
    ok("\"hi\"", "\"hi\"");
    ok("\"a\\nb\\t\\\"c\\\"\"", "\"a\\nb\\t\\\"c\\\"\"");   /* escapes preserved verbatim */
    ok("\"\\u00e9\"", "\"\\u00e9\"");

    /* empty containers */
    ok("{}", "{}");
    ok("[]", "[]");
    ok("[ ]", "[]");
    ok("{ }", "{}");

    /* arrays */
    ok("[1,2,3]", "[\n  1,\n  2,\n  3\n]");
    ok("[ 1 , 2 ]", "[\n  1,\n  2\n]");

    /* objects */
    ok("{\"a\":1}", "{\n  \"a\": 1\n}");
    ok("{ \"a\" : 1 , \"b\" : 2 }", "{\n  \"a\": 1,\n  \"b\": 2\n}");

    /* nesting */
    ok("{\"a\":[1,{\"b\":true}],\"c\":null}",
       "{\n  \"a\": [\n    1,\n    {\n      \"b\": true\n    }\n  ],\n  \"c\": null\n}");
    ok("[[],{}]", "[\n  [],\n  {}\n]");

    /* --- errors, checked by byte offset -------------------------------------*/
    bad("", 0);                 /* empty input: no value */
    bad("{", 1);                /* object needs a key or '}' */
    bad("[", 1);                /* array needs a value or ']' */
    bad("[1,]", 3);             /* trailing comma -> expects a value at offset 3 */
    bad("{\"a\":1,}", 7);       /* trailing comma in object */
    bad("{a:1}", 1);            /* unquoted key */
    bad("{\"a\" 1}", 5);        /* missing colon */
    bad("[1 2]", 3);            /* missing comma */
    bad("truex", 4);            /* trailing junk after a keyword */
    bad("42 43", 3);            /* two top-level values */
    bad("01", 1);               /* leading zero: '0' parses, '1' is trailing junk */
    bad("1.", 2);               /* fraction needs a digit */
    bad("\"abc", 4);            /* unterminated string */
    bad("\"a\\x\"", 3);         /* bad escape */
    bad("nul", 0);              /* not a full keyword */

    /* deep nesting past the cap is rejected (not a crash) */
    checks++;
    {
        char deep[200]; int n = 0;
        for (int i = 0; i < 80; i++) deep[n++] = '[';
        deep[n] = 0;
        char out[4096];
        int e = json_format(deep, out, sizeof out);
        if (e < 0) { printf("  FAIL deep nesting should be rejected\n"); fails++; }
    }

    /* --- minify (M1725): same validation, compact output ------------------*/
    mini("42", "42");
    mini("  true ", "true");
    mini("\"a b\"", "\"a b\"");                          /* spaces INSIDE strings preserved */
    mini("[1,2,3]", "[1,2,3]");
    mini("[ 1 , 2 ]", "[1,2]");
    mini("{}", "{}");
    mini("[]", "[]");
    mini("{\"a\":1}", "{\"a\":1}");
    mini("{ \"a\" : 1 , \"b\" : 2 }", "{\"a\":1,\"b\":2}");
    mini("{\n  \"x\": [1, 2],\n  \"y\": {\"z\": 3}\n}", "{\"x\":[1,2],\"y\":{\"z\":3}}");   /* strips all layout whitespace */
    mini("{\"k\":\"v v\",\"n\":[true, null, -3.5e2]}", "{\"k\":\"v v\",\"n\":[true,null,-3.5e2]}");
    /* minify is idempotent, and re-pretty-printing a minified doc round-trips */
    {
        char m[4096], p[4096], m2[4096];
        const char *doc = "{ \"a\":[1,2,{\"b\":3}], \"c\":\"hi\" }";
        json_minify(doc, m, sizeof m);
        json_format(m, p, sizeof p);        /* minified -> pretty (valid) */
        json_minify(p, m2, sizeof m2);      /* pretty  -> minified again */
        checks++; if (strcmp(m, m2) != 0) { printf("  FAIL minify round-trip: \"%s\" vs \"%s\"\n", m, m2); fails++; }
    }

    if (!fails) printf("PASS: %d checks, JSON engine correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", fails, checks);
    return fails ? 1 : 0;
}
