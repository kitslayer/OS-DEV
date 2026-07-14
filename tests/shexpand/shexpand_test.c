/* shexpand_test.c — host-side regression + fuzz for the shell's parameter/variable
 * expander (user/shexpand.h: expand_vars). Built with ASan+UBSan. The pass is pure
 * apart from three hooks we stub here — vget (variable lookup), sh_laststatus ($?),
 * and shmath's sh_var — so it is unit-testable off-target like shbrace/shgrep/shmath.
 * Exit 0 = pass.
 *
 * Keep the expected expansions in sync with how run_line applies expand_vars. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "shgrep.h"     /* glob_match (pure) — used by ${VAR#pat}/${VAR%pat} */

/* --- hooks the expander (and shmath) need ------------------------------------ */
static const struct { const char *n, *v; } g_vars[] = {
    {"FOO",   "bar"},
    {"EMPTY", ""},
    {"NUM",   "42"},
    {"PATH",  "/usr/local/bin"},        /* length 14 */
    {"HELLO", "Hello World"},           /* length 11, for substring slices */
    {"FILE",  "archive.tar.gz"},
    {"BANANA","banana"},
    {"@",     "one two three"},         /* $@ / $* / ${#} share the "@"/"#" pseudo-vars */
    {"#",     "3"},
    {0, 0}
};
/* mutable overlay so ${VAR:=default} assignments (via vset) are observable */
static struct { char n[24]; char v[64]; } g_set[16];
static int g_nset;
static void vset(const char *n, int nl, const char *v){
    if (nl > 23) nl = 23;
    for (int i = 0; i < g_nset; i++)
        if ((int)strlen(g_set[i].n) == nl && !strncmp(g_set[i].n, n, (size_t)nl)) {
            strncpy(g_set[i].v, v, 63); g_set[i].v[63] = 0; return;
        }
    if (g_nset < 16) {
        int k = 0; for (; k < nl; k++) g_set[g_nset].n[k] = n[k]; g_set[g_nset].n[k] = 0;
        strncpy(g_set[g_nset].v, v, 63); g_set[g_nset].v[63] = 0; g_nset++;
    }
}
static const char *vget(const char *n, int nl){
    for (int i = 0; i < g_nset; i++)               /* overlay wins (assigned vars) */
        if ((int)strlen(g_set[i].n) == nl && !strncmp(g_set[i].n, n, (size_t)nl)) return g_set[i].v;
    for (int i = 0; g_vars[i].n; i++){
        int L = (int)strlen(g_vars[i].n);
        if (L == nl && !strncmp(g_vars[i].n, n, (size_t)nl)) return g_vars[i].v;
    }
    return 0;
}
static int g_last_status = 0;
static int sh_laststatus(void){ return g_last_status < 0 ? 0 : g_last_status; }

#include "shmath.h"     /* sh_eval/sh_askip/sh_vchar; needs sh_var below */
static long sh_var(const char *name, int len){ const char *v = vget(name, len); return v ? sh_str2long(v) : 0; }

#include "shexpand.h"   /* expand_vars — the unit under test */

static int fails = 0;
/* expand_vars returns 0 (dst untouched) when there is no '$'; in that case the
 * result is the input itself. Normalize so CHECK works for both. */
#define CHECK(in, want) do { char got[1024]; \
    if (!expand_vars(in, got, (int)sizeof got)) { strncpy(got, in, sizeof got - 1); got[sizeof got - 1] = 0; } \
    if (strcmp(got, want)) { printf("FAIL: %-30s -> [%s] (want [%s])\n", in, got, want); fails++; } } while (0)

int main(void){
    /* --- plain $NAME / ${NAME} --- */
    CHECK("$FOO", "bar");
    CHECK("${FOO}", "bar");
    CHECK("a${FOO}b", "abarb");
    CHECK("$FOO$FOO", "barbar");
    CHECK("x=$NUM", "x=42");
    CHECK("$UNSET", "");                     /* unset -> empty */
    CHECK("${UNSET}", "");
    CHECK("[${UNSET}]", "[]");
    CHECK("no dollars here", "no dollars here");
    CHECK("", "");

    /* --- $?, $#, $@, $* --- */
    g_last_status = 0;  CHECK("$?", "0");
    g_last_status = 7;  CHECK("$?", "7");
    g_last_status = 42; CHECK("exit $?", "exit 42");
    g_last_status = 0;
    CHECK("$#", "3");
    CHECK("$@", "one two three");
    CHECK("$*", "one two three");            /* $* == $@ in this shell (M1750) */

    /* --- ${#NAME} length --- */
    CHECK("${#FOO}", "3");
    CHECK("${#PATH}", "14");
    CHECK("${#UNSET}", "0");
    CHECK("${#}", "3");                       /* bare ${#} = arg count */

    /* --- ${VAR:-word} default / ${VAR:+word} alternate --- */
    CHECK("${FOO:-def}", "bar");             /* set -> value */
    CHECK("${UNSET:-def}", "def");           /* unset -> default */
    CHECK("${EMPTY:-def}", "def");           /* empty counts as unset for :- */
    CHECK("${FOO:+alt}", "alt");             /* set -> alternate */
    CHECK("${UNSET:+alt}", "");              /* unset -> empty */
    CHECK("${EMPTY:+alt}", "");              /* empty -> empty */

    /* --- ${NAME#pat} / ##  prefix strip (glob) --- */
    CHECK("${FILE#*.}", "tar.gz");           /* shortest matching prefix "archive." */
    CHECK("${FILE##*.}", "gz");              /* longest matching prefix "archive.tar." */
    CHECK("${FILE#archive.}", "tar.gz");     /* literal prefix */
    CHECK("${FILE#nomatch}", "archive.tar.gz"); /* no match -> unchanged */

    /* --- ${NAME%pat} / %%  suffix strip (glob) --- */
    CHECK("${FILE%.*}", "archive.tar");      /* shortest matching suffix ".gz" */
    CHECK("${FILE%%.*}", "archive");         /* longest matching suffix ".tar.gz" */
    CHECK("${FILE%.gz}", "archive.tar");     /* literal suffix */
    CHECK("${FILE%nomatch}", "archive.tar.gz");

    /* --- ${VAR/pat/repl} first / ${VAR//pat/repl} all (M1803) --- */
    CHECK("${FILE/tar/TAR}", "archive.TAR.gz");   /* first literal */
    CHECK("${FILE/./_}", "archive_tar.gz");       /* first '.' */
    CHECK("${FILE//./_}", "archive_tar_gz");      /* all '.' */
    CHECK("${FILE//a/A}", "Archive.tAr.gz");      /* all 'a' */
    CHECK("${FILE/gz/}", "archive.tar.");         /* empty replacement = delete */
    CHECK("${FILE/*.gz/X}", "X");                 /* glob '*' matches greedily */
    CHECK("${FILE/nomatch/Y}", "archive.tar.gz"); /* no match -> unchanged */
    CHECK("${PATH//x/y}", "/usr/local/bin");      /* no match -> unchanged */
    CHECK("${PATH//\\//_}", "_usr_local_bin");    /* replace all '/' (escaped pattern slash) */
    CHECK("${BANANA/a/X}", "bXnana");             /* first 'a' */
    CHECK("${BANANA//a/X}", "bXnXnX");            /* all 'a' */
    CHECK("${BANANA//a*/Z}", "bZ");               /* greedy glob, first match reaches end */
    CHECK("${BANANA//}", "banana");               /* empty pattern -> no-op */

    /* --- ${VAR:off} / ${VAR:off:len} substring (M1804), HELLO="Hello World" (n=11) --- */
    CHECK("${HELLO:6}", "World");                 /* offset to end */
    CHECK("${HELLO:0:5}", "Hello");               /* offset + length */
    CHECK("${HELLO:6:5}", "World");
    CHECK("${HELLO:6:100}", "World");             /* length clamps to string end */
    CHECK("${HELLO: -5}", "World");               /* negative offset (space disambiguates from :-) */
    CHECK("${HELLO: -5:2}", "Wo");                /* negative offset + length */
    CHECK("${HELLO:0:-6}", "Hello");              /* negative length = end measured from the end */
    CHECK("${HELLO:2:-2}", "llo Wor");            /* both interior */
    CHECK("${HELLO:100}", "");                    /* offset past end -> empty */
    CHECK("${HELLO:5:0}", "");                    /* zero length -> empty */
    CHECK("${HELLO:-3}", "Hello World");          /* ${VAR:-word}: HELLO is set -> its value (NOT a slice) */
    CHECK("${UNSET:-3}", "3");                     /* ${VAR:-word}: default "3" (regression guard for the :- vs :off split) */

    /* --- ${VAR:=default} assign-default (M1805): assigns via vset when unset/empty --- */
    CHECK("${FOO:=x}", "bar");                    /* set -> value, no assignment */
    CHECK("$FOO", "bar");                          /* ...and FOO is unchanged */
    CHECK("${NEW:=hello}", "hello");              /* unset -> assign + expand */
    CHECK("$NEW", "hello");                        /* assignment persisted (overlay) */
    CHECK("${NEW:=other}", "hello");              /* now set -> keeps first assignment */
    CHECK("${EMPTY:=def}", "def");                /* empty counts as unset -> assign */
    CHECK("$EMPTY", "def");                        /* assignment persisted */

    /* --- ${VAR^^}/${VAR^} upper, ${VAR,,}/${VAR,} lower case conversion (M1821) --- */
    CHECK("${FOO^^}", "BAR");                      /* all upper */
    CHECK("${FOO^}", "Bar");                       /* first char only */
    CHECK("${HELLO,,}", "hello world");           /* all lower */
    CHECK("${HELLO,}", "hello World");            /* first char only */
    CHECK("${BANANA^^}", "BANANA");
    CHECK("${HELLO^^}", "HELLO WORLD");
    CHECK("${FOO,,}", "bar");                      /* already lower -> unchanged */
    CHECK("${NUM^^}", "42");                       /* digits unaffected */
    CHECK("${UNSET^^}", "");                       /* unset -> empty */
    CHECK("x${FOO^^}y", "xBARy");                  /* embedded in a word */

    /* --- $((expr)) arithmetic (delegates to shmath) --- */
    CHECK("$((2+3))", "5");
    CHECK("$((NUM*2))", "84");               /* NUM=42 via sh_var */
    CHECK("count=$((1+2+3))", "count=6");

    /* --- combinations --- */
    CHECK("$FOO/${NUM}/${#PATH}", "bar/42/14");
    CHECK("${UNSET:-$FOO}", "$FOO");         /* default word is emitted literally (single pass, no nested expand) — locks current behavior */

    if (fails) { printf("\n%d regression case(s) FAILED\n", fails); return 1; }
    printf("all parameter-expansion regression cases passed\n");

    /* --- fuzz: random $-ish strings must never crash / OOB / hang under ASan --- */
    const char alph[] = "$ { } : - + # % ( ) * @ ? / . abcFOO123NUM";
    unsigned seed = 0x51ade5;
    for (int t = 0; t < 200000; t++){
        char in[64]; int n = (int)(seed % 40);
        for (int k = 0; k < n; k++){ seed = seed * 1103515245u + 12345u; in[k] = alph[(seed >> 8) % (sizeof alph - 1)]; }
        in[n] = 0;
        char out[128];
        g_last_status = (int)(seed % 300) - 10;   /* exercise the $? clamp too */
        expand_vars(in, out, (int)sizeof out);
        /* also a tiny cap to prove the o<cap-1 bound never overflows */
        char tiny[4]; expand_vars(in, tiny, (int)sizeof tiny);
        seed = seed * 1103515245u + 12345u;
    }
    printf("fuzz: 200000 random inputs expanded without crash/OOB\n");
    return 0;
}
