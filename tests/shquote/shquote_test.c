/* shquote_test.c — host-side regression + fuzz for the shell's quoting pass
 * (user/shquote.h: sh_quote_pass / sh_unprot_buf). Built with ASan+UBSan. Pure,
 * so it's unit-testable off-target like shsplit/shbrace. Exit 0 = pass.
 *
 * Expected strings use \xNN escapes for the protected (bit-7-set) bytes, e.g. a
 * protected space is \xa0 (0x20|0x80), protected ';' is \xbb, '$' is \xa4. */
#include <stdio.h>
#include <string.h>
#include "shquote.h"

static int fails = 0;
/* sh_quote_pass rewrites in place; copy `in` into a buffer first. */
#define CHECK(in, want) do { char b[512]; strcpy(b, in); sh_quote_pass(b); \
    if (strcmp(b, want)) { printf("FAIL: [%s] -> [%s] (want [%s])\n", in, b, want); fails++; } } while (0)

int main(void) {
    /* --- spaces inside quotes are protected (one argument) --- */
    CHECK("echo \"a b\"", "echo a\xa0" "b");      /* double-quote space -> \xa0 */
    CHECK("echo 'a b'",   "echo a\xa0" "b");      /* single-quote space  */
    CHECK("echo unq arg", "echo unq arg");        /* unquoted: untouched  */

    /* --- metacharacters inside quotes are protected (literal) --- */
    CHECK("echo \"a;b\"", "echo a\xbb" "b");      /* ; */
    CHECK("echo 'a;b'",   "echo a\xbb" "b");
    CHECK("echo \"x|y\"", "echo x\xfc" "y");      /* | */
    CHECK("echo \"a&b\"", "echo a\xa6" "b");      /* & */
    CHECK("echo \"a<b>c\"", "echo a\xbc" "b\xbe" "c");  /* < > */
    CHECK("echo \"*\"",   "echo \xaa");           /* * (glob) protected in double */
    CHECK("echo '*.c'",   "echo \xaa.c");

    /* --- $ : expands in double quotes (left literal), not in single (protected) --- */
    CHECK("echo \"$x\"",  "echo $x");             /* $ stays literal -> expand_vars fires */
    CHECK("echo '$x'",    "echo \xa4x");          /* $ protected -> no expansion */
    CHECK("echo \"${y}\"", "echo ${y}");          /* ${...} preserved in double quotes */

    /* --- braces: protected (literal) only in single quotes --- */
    CHECK("echo '{a,b}'", "echo \xfb" "a\xac" "b\xfd");   /* { , } protected */
    CHECK("echo \"{a,b}\"", "echo {a,b}");        /* left literal in double quotes */

    /* --- the OTHER quote kind inside a quote stays a literal (protected) char --- */
    CHECK("echo \"it's\"", "echo it\xa7s");       /* ' inside "..." -> protected literal (0x27|0x80) */
    CHECK("echo 'say \"hi\"'", "echo say\xa0\xa2" "hi\xa2");   /* " inside '...' protected */

    /* --- quote chars are removed; backslash escapes inside double quotes --- */
    CHECK("a\"b\"c",      "abc");                 /* adjacent quoting concatenates */
    CHECK("echo \"a\\\"b\"", "echo a\xa2" "b");   /* \" -> a literal (protected) double-quote */
    CHECK("echo \"a\\\\b\"", "echo a\xdc" "b");   /* \\ -> a literal (protected) backslash (0x5c|0x80) */

    /* --- edges --- */
    CHECK("", "");
    CHECK("echo \"open", "echo open");            /* unbalanced: implicit close at EOL */
    CHECK("''", "");                              /* empty quotes vanish */
    CHECK("'a''b'", "ab");

    if (fails) { printf("\n%d regression case(s) FAILED\n", fails); return 1; }
    printf("all shquote regression cases passed\n");

    /* --- fuzz: random quote-heavy input must never crash / OOB, must strip every
     * quote char, and the result must fully un-protect (no stray high-bit byte). --- */
    const char alpha[] = "\"'$;|&<>*?(){}, ab\\`";
    unsigned int seed = 0x12345678u;
    for (long iter = 0; iter < 3000000; iter++) {
        char in[40]; int n = (seed >> 6) % 32;
        for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; in[i] = alpha[(seed >> 8) % (int)(sizeof alpha - 1)]; }
        in[n] = 0;
        char out[40]; memcpy(out, in, n + 1);
        sh_quote_pass(out);
        int olen = (int)strlen(out);
        if (olen > n) { printf("FAIL: output grew for [%s]\n", in); return 1; }   /* the pass only deletes/marks, never grows */
        for (int i = 0; i < olen; i++)
            if (out[i] == '"' || out[i] == '\'') { printf("FAIL: quote char survived in [%s]\n", in); return 1; }
        sh_unprot_buf(out);                       /* must fully reveal */
        for (int i = 0; out[i]; i++)
            if (SH_ISPROT(out[i])) { printf("FAIL: byte stayed protected in [%s]\n", in); return 1; }
        seed = seed * 1103515245u + 12345u;
    }
    printf("fuzz: 3000000 random inputs, no crash / OOB, all quotes stripped, all bytes un-protect\n");
    return 0;
}
