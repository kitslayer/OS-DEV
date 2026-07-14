/* shtxt_test.c — host-side regression for the shell's tr/cut text helpers
 * (user/shtxt.h): tr SET-token expansion and cut range membership. Pure, built
 * for the host under ASan+UBSan. Exit 0 = pass. Keep in sync with user/shtxt.h. */
#include <stdio.h>
#include <string.h>
#include "shtxt.h"

static int fails = 0, checks = 0;
#define EQS(got, want) do { checks++; const char *g=(got); if (strcmp(g,(want))!=0) { printf("FAIL: got \"%s\" want \"%s\"\n",g,(want)); fails++; } } while(0)
#define EQL(got, want) do { checks++; long g=(long)(got); if (g!=(long)(want)) { printf("FAIL: got %ld want %ld\n",g,(long)(want)); fails++; } } while(0)

static int trx(const char *in, char *out) { const char *p = in; int n = tr_expand(&p, out, 128); out[n] = 0; return n; }

int main(void) {
    char b[160];
    /* --- tr_expand: literal chars + a-z ranges, stops at a space --- */
    EQL(trx("abc", b), 3);        EQS(b, "abc");
    EQL(trx("a-c", b), 3);        EQS(b, "abc");
    EQL(trx("a-cx", b), 4);       EQS(b, "abcx");
    EQL(trx("0-9", b), 10);       EQS(b, "0123456789");
    EQL(trx("A-Ea", b), 6);       EQS(b, "ABCDEa");
    EQL(trx("abc def", b), 3);    EQS(b, "abc");            /* stops at the space */
    EQL(trx("a-", b), 2);         EQS(b, "a-");             /* trailing '-' is literal */
    EQL(trx("z-a", b), 3);        EQS(b, "z-a");            /* reverse range -> literals */
    EQL(trx("x", b), 1);          EQS(b, "x");
    /* tr_expand advances the cursor past the token */
    { const char *p = "abc def"; tr_expand(&p, b, 128); checks++;
      if (strcmp(p, " def") != 0) { printf("FAIL: cursor at \"%s\" want \" def\"\n", p); fails++; } }

    /* --- cut_sel: membership in [rf,rt] ranges (oe = open-ended) --- */
    { int rf[]={1,5}, rt[]={2,7}, oe[]={0,0};
      EQL(cut_sel(1, rf,rt,oe,2), 1); EQL(cut_sel(2, rf,rt,oe,2), 1);
      EQL(cut_sel(3, rf,rt,oe,2), 0); EQL(cut_sel(4, rf,rt,oe,2), 0);
      EQL(cut_sel(5, rf,rt,oe,2), 1); EQL(cut_sel(7, rf,rt,oe,2), 1);
      EQL(cut_sel(8, rf,rt,oe,2), 0); }
    { int rf[]={3}, rt[]={0}, oe[]={1};                     /* open-ended [3, inf) */
      EQL(cut_sel(2, rf,rt,oe,1), 0); EQL(cut_sel(3, rf,rt,oe,1), 1); EQL(cut_sel(100, rf,rt,oe,1), 1); }

    if (fails) { printf("shtxt: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("shtxt: all %d tr/cut helper checks passed\n", checks);
    return 0;
}
