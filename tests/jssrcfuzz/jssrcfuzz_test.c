/*
 * jssrcfuzz_test.c — host-side fuzz of the full JS parse+run pipeline on
 * untrusted SOURCE (ASan + UBSan).
 *
 * The engine's primary untrusted input is the page script itself: the browser
 * runs `<script>` and javascript: URLs from arbitrary sites, in-kernel on a
 * guard-page-less stack. So malformed/truncated/adversarial source must never
 * read out of bounds, overflow the stack, or hang — it must fail gracefully
 * (a `[js error: …]`) at worst. The lexer/parser bounds and the MAXDEPTH parse
 * guard + the loop-iteration / recursion / arena-OOM run guards must hold.
 *
 * This #includes js.c (JS_NO_MAIN), wires up the same host DOM/storage stubs the
 * normal host runner uses (so a random script touching document/window can't
 * null-deref), and feeds js_run_doc every truncated prefix + single-byte
 * corruption of a rich script, 200k random buffers, and deep nesting. Exit 0 = pass.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define JS_HOSTTEST 1
#define JS_NO_MAIN 1
#include "js.c"

static char g_outbuf[16384];

static void setup_stubs(void) {
    js_set_storage(host_get, host_set, host_remove, host_clear);
    js_set_dom(hdom_get, hdom_set);
    js_set_dom_attr(hdom_getattr, hdom_setattr);
    js_set_dom_pos(hdom_get_at, hdom_set_at, hdom_getattr_at, hdom_setattr_at, hdom_query);
    js_set_dom_match(hdom_matches, hdom_matches_at, hdom_closest, hdom_closest_at);
    js_set_dom_children(hdom_children, hdom_children_at, hdom_parent, hdom_parent_at, hdom_sibling, hdom_sibling_at);
    js_set_dom_tag(hdom_tag, hdom_tag_at);
    js_set_dom_rmattr(hdom_rmattr, hdom_rmattr_at);
    js_set_location("https://host.example/dir/page?q=hi&n=2");
}

/* run `src` (len bytes) through the whole pipeline from an exactly-sized,
 * NUL-terminated heap buffer so any over-read past the source trips a red-zone. */
static void fuzz_one(const char *src, int len) {
    char *s = malloc(len + 1); memcpy(s, src, len); s[len] = 0;
    js_run_doc(s, g_outbuf, (int)sizeof(g_outbuf), 0);   /* output ignored; memory + termination safety only */
    free(s);
}

int main(void) {
    setup_stubs();

    /* a couple of sanity runs (valid scripts complete without a fault) */
    fuzz_one("var x=1; for(let i=0;i<3;i++) x+=i; print(x);", 45);
    fuzz_one("try{ JSON.parse('{bad'); }catch(e){ print('caught'); }", 53);

    const char *valid =
        "function f(a,b){ return a+b; } var o={x:1,y:[1,2,3]}; "
        "for(let i=0;i<o.y.length;i++){ o.x+=o.y[i]; } "
        "var s=[1,2,3].map(n=>n*2).filter(n=>n>2).join(','); "
        "var r=/(\\d+)-(\\d+)/.exec('12-34'); print(f(o.x,s.length)); "
        "class C{ constructor(v){this.v=v;} get d(){return this.v*2;} } print(new C(5).d);";
    int vlen = (int)strlen(valid);

    /* every truncated prefix */
    for (int len = 0; len <= vlen; len++) fuzz_one(valid, len);

    /* single-byte corruptions (sampled across the source) */
    for (int pos = 0; pos < vlen; pos++)
        for (int v = 1; v <= 256; v += 5) {       /* step 5: ~52 mutations/byte, keeps the run quick */
            char *t = malloc(vlen); memcpy(t, valid, vlen); t[pos] ^= (char)v;
            fuzz_one(t, vlen); free(t);
        }

    /* random buffers biased toward JS tokens so the parser's structural paths run */
    srand(20260617);
    const char *toks = "(){}[];,.=+-*/%<>!&|?:'\"`\\ \t\n0123abcfnrtuvwxyz_$";
    int tlen = (int)strlen(toks);
    for (int trial = 0; trial < 200000; trial++) {
        int len = rand() % 128;
        char buf[128];
        for (int i = 0; i < len; i++) buf[i] = (trial & 1) ? toks[rand() % tlen] : (char)(33 + rand() % 94);
        fuzz_one(buf, len);
    }

    /* deeply nested source must hit the MAXDEPTH parse guard, never overflow */
    for (int depth = 1; depth <= 4000; depth += 9) {
        char *deep = malloc(depth * 2 + 8);
        int p = 0; for (int i = 0; i < depth; i++) deep[p++] = '(';
        deep[p++] = '1'; for (int i = 0; i < depth; i++) deep[p++] = ')'; deep[p] = 0;
        fuzz_one(deep, p); free(deep);
    }

    printf("fuzz: truncations + corruptions + 200000 random + deep nesting -> all clean\n");
    printf("PASS: JS source parse+run pipeline (malformed/adversarial fuzz, ASan/UBSan clean, no hang)\n");
    return 0;
}
