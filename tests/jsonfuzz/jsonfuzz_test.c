/*
 * jsonfuzz_test.c — host-side fuzz of the engine's JSON.parse (ASan + UBSan).
 *
 * The browser feeds JSON.parse raw bytes from untrusted servers/APIs, and the
 * parser runs in-kernel on a guard-page-less stack, so a malformed/truncated/
 * deeply-nested document must never read out of bounds or overflow the stack.
 * This #includes js.c (JS_HOSTTEST) and drives nat_json_parse directly: every
 * truncated prefix of a valid document, every single-byte corruption, many
 * random buffers (biased toward JSON punctuation), and pathologically nested
 * brackets — each in an exactly-sized NUL-terminated buffer so ASan red-zones
 * any over-read. A few valid documents confirm correct parsing. Exit 0 = pass.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define JS_HOSTTEST 1
#define JS_NO_MAIN 1     /* we supply our own main; suppress js.c's host main */
#include "js.c"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", (msg)); fails++; } } while (0)

/* reset the engine's per-run state, then parse `buf` (len bytes) via JSON.parse */
static val parse_buf(const char *buf, int len) {
    g_arena_off = 0; g_oom = 0; g_err = 0; g_threw = 0; g_errmsg[0] = 0; g_depth = 0;
    char *s = (char *)aalloc(len + 1);            /* copy into the arena, NUL-terminated */
    if (!s) return UND();
    memcpy(s, buf, len); s[len] = 0;
    val arg = STRV(s);
    return nat_json_parse(&arg, 1);
}

/* ASan-guarded variant: parse from an exactly-sized heap buffer so any read past
 * the input (beyond the NUL) trips a red-zone. */
static void fuzz_one(const unsigned char *data, int len) {
    char *b = malloc(len + 1);
    memcpy(b, data, len); b[len] = 0;
    g_arena_off = 0; g_oom = 0; g_err = 0; g_threw = 0; g_errmsg[0] = 0; g_depth = 0;
    val arg = STRV(b);
    nat_json_parse(&arg, 1);     /* result discarded; we only care about memory safety */
    free(b);
}

int main(void) {
    /* ---- regression: valid documents parse correctly ---- */
    val r;
    r = parse_buf("42", 2);                 CHECK(r.t == V_NUM && r.num == 42, "number");
    r = parse_buf("-7", 2);                 CHECK(r.t == V_NUM && r.num == -7, "neg number");
    r = parse_buf("true", 4);               CHECK(r.t == V_BOOL && r.num == 1, "true");
    r = parse_buf("null", 4);               CHECK(r.t == V_NULL, "null");
    r = parse_buf("\"hi\"", 4);             CHECK(r.t == V_STR && strcmp(r.str, "hi") == 0, "string");
    r = parse_buf("[1,2,3]", 7);            CHECK(r.t == V_ARR && r.o && r.o->n == 3, "array");
    r = parse_buf("{\"a\":1,\"b\":2}", 13); CHECK(r.t == V_OBJ && r.o, "object");
    r = parse_buf("{\"a\":[1,{\"b\":2}]}", 17); CHECK(r.t == V_OBJ && r.o, "nested");
    /* malformed must set the error (not crash) */
    parse_buf("{\"a\":", 5);                CHECK(g_err, "unclosed object flagged");
    parse_buf("", 0);                       CHECK(g_err, "empty flagged");
    printf("regression: %s\n", fails ? "FAILURES" : "ok (valid parse + malformed flagged)");

    /* ---- fuzz: every truncated prefix of a rich valid document ---- */
    const char *valid = "{\"name\":\"x\",\"n\":-12.5,\"ok\":true,\"z\":null,\"a\":[1,2,[3,{\"k\":\"v\\n\\\"\"}]],\"o\":{\"p\":{\"q\":[]}}}";
    int vlen = (int)strlen(valid);
    for (int len = 0; len <= vlen; len++) fuzz_one((const unsigned char *)valid, len);

    /* every single-byte corruption */
    for (int pos = 0; pos < vlen; pos++)
        for (int v = 1; v <= 256; v++) {
            char tmp[256]; memcpy(tmp, valid, vlen); tmp[pos] ^= (char)v;
            fuzz_one((const unsigned char *)tmp, vlen);
        }

    /* random buffers, biased toward JSON punctuation so the structural paths run */
    srand(424242);
    const char *alpha = "{}[]\":,0123456789tfn-.\\ \tabce";
    int alen = (int)strlen(alpha);
    for (int trial = 0; trial < 300000; trial++) {
        int len = rand() % 160;
        unsigned char tmp[160];
        for (int i = 0; i < len; i++) tmp[i] = (trial & 1) ? (unsigned char)alpha[rand() % alen] : (unsigned char)rand();
        fuzz_one(tmp, len);
    }

    /* pathological nesting must hit the depth guard, never overflow the stack */
    for (int depth = 1; depth <= 5000; depth += 7) {
        char *deep = malloc(depth + 1);
        for (int i = 0; i < depth; i++) deep[i] = '[';
        deep[depth] = 0;
        fuzz_one((const unsigned char *)deep, depth);
        free(deep);
    }

    printf("fuzz: truncations + single-byte corruptions + 300000 random + deep-nesting -> %s\n",
           fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: JSON.parse (valid parse + malformed/deep fuzz, ASan/UBSan clean)\n");
    return 0;
}
