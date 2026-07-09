/* jsoncore.h — a pure JSON validator + pretty-printer (M1703).
 *
 * Recursive-descent over RFC-8259 JSON, re-emitting the input with 2-space
 * indentation. No allocation, no syscalls, and no floating point — number
 * tokens are copied verbatim (so precision is preserved and the app can build
 * without SSE). Because it is pure, it is host-unit-tested by tests/json exactly
 * like calc's calceval.h, sheet's sheeteval.h and plot's ploteval.h. The single
 * entry point, json_format(), validates and pretty-prints in one pass, returning
 * the byte offset of the first syntax error (or -1 on success).
 */
#ifndef JSONCORE_H
#define JSONCORE_H

#define JC_MAXDEPTH 64                 /* nesting cap (guards the recursion) */

static const char *jc_base;            /* start of input (for error offsets) */
static const char *jc_p;               /* parse cursor */
static char       *jc_out;             /* output buffer */
static int         jc_cap;             /* output capacity */
static int         jc_len;             /* bytes emitted so far */
static int         jc_err;             /* offset of first error, -1 = ok */
static int         jc_depth;

static int  jc_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static void jc_emit(char c) { if (jc_len < jc_cap - 1) jc_out[jc_len++] = c; }
static void jc_emits(const char *s) { while (*s) jc_emit(*s++); }
static int  jc_compact;                /* 1 = minify (no indentation / newlines / ': ' space) */
static void jc_indent(int n) { if (jc_compact) return; for (int i = 0; i < n; i++) { jc_emit(' '); jc_emit(' '); } }
static void jc_nl(void)      { if (!jc_compact) jc_emit('\n'); }
static void jc_ws(void) { while (*jc_p == ' ' || *jc_p == '\t' || *jc_p == '\n' || *jc_p == '\r') jc_p++; }
static void jc_fail(void) { if (jc_err < 0) jc_err = (int)(jc_p - jc_base); }

static void jc_value(int ind);

/* Copy a JSON string token verbatim while validating its escapes. */
static void jc_string(void) {
    if (*jc_p != '"') { jc_fail(); return; }
    jc_emit('"'); jc_p++;
    while (*jc_p && *jc_p != '"') {
        if (*jc_p == '\\') {
            jc_emit('\\'); jc_p++;
            char e = *jc_p;
            if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') { jc_emit(e); jc_p++; }
            else if (e == 'u') { jc_emit('u'); jc_p++; for (int i = 0; i < 4; i++) { if (!jc_hex(*jc_p)) { jc_fail(); return; } jc_emit(*jc_p++); } }
            else { jc_fail(); return; }
        } else if ((unsigned char)*jc_p < 0x20) { jc_fail(); return; }   /* an unescaped control char is invalid */
        else jc_emit(*jc_p++);
    }
    if (*jc_p != '"') { jc_fail(); return; }
    jc_emit('"'); jc_p++;
}

/* Copy a JSON number token verbatim while validating its shape. */
static void jc_number(void) {
    const char *s = jc_p;
    if (*jc_p == '-') jc_p++;
    if (*jc_p == '0') jc_p++;
    else if (*jc_p >= '1' && *jc_p <= '9') { while (*jc_p >= '0' && *jc_p <= '9') jc_p++; }
    else { jc_fail(); return; }
    if (*jc_p == '.') { jc_p++; if (!(*jc_p >= '0' && *jc_p <= '9')) { jc_fail(); return; } while (*jc_p >= '0' && *jc_p <= '9') jc_p++; }
    if (*jc_p == 'e' || *jc_p == 'E') { jc_p++; if (*jc_p == '+' || *jc_p == '-') jc_p++; if (!(*jc_p >= '0' && *jc_p <= '9')) { jc_fail(); return; } while (*jc_p >= '0' && *jc_p <= '9') jc_p++; }
    for (const char *q = s; q < jc_p; q++) jc_emit(*q);
}

static void jc_value(int ind) {
    if (jc_err >= 0) return;
    if (++jc_depth > JC_MAXDEPTH) { jc_fail(); jc_depth--; return; }
    jc_ws();
    char c = *jc_p;
    if (c == '"') jc_string();
    else if (c == '{') {
        jc_p++; jc_ws();
        if (*jc_p == '}') { jc_emits("{}"); jc_p++; jc_depth--; return; }
        jc_emit('{'); jc_nl();
        for (;;) {
            jc_ws();
            jc_indent(ind + 1);
            if (*jc_p != '"') { jc_fail(); break; }         /* keys must be strings */
            jc_string();
            jc_ws();
            if (*jc_p != ':') { jc_fail(); break; }
            jc_p++; jc_emit(':'); if (!jc_compact) jc_emit(' ');
            jc_value(ind + 1);
            if (jc_err >= 0) break;
            jc_ws();
            if (*jc_p == ',') { jc_p++; jc_emit(','); jc_nl(); continue; }
            if (*jc_p == '}') { jc_nl(); jc_indent(ind); jc_emit('}'); jc_p++; break; }
            jc_fail(); break;
        }
    }
    else if (c == '[') {
        jc_p++; jc_ws();
        if (*jc_p == ']') { jc_emits("[]"); jc_p++; jc_depth--; return; }
        jc_emit('['); jc_nl();
        for (;;) {
            jc_indent(ind + 1);
            jc_value(ind + 1);
            if (jc_err >= 0) break;
            jc_ws();
            if (*jc_p == ',') { jc_p++; jc_emit(','); jc_nl(); continue; }
            if (*jc_p == ']') { jc_nl(); jc_indent(ind); jc_emit(']'); jc_p++; break; }
            jc_fail(); break;
        }
    }
    else if (c == 't') { if (jc_p[1] == 'r' && jc_p[2] == 'u' && jc_p[3] == 'e') { jc_emits("true"); jc_p += 4; } else jc_fail(); }
    else if (c == 'f') { if (jc_p[1] == 'a' && jc_p[2] == 'l' && jc_p[3] == 's' && jc_p[4] == 'e') { jc_emits("false"); jc_p += 5; } else jc_fail(); }
    else if (c == 'n') { if (jc_p[1] == 'u' && jc_p[2] == 'l' && jc_p[3] == 'l') { jc_emits("null"); jc_p += 4; } else jc_fail(); }
    else if (c == '-' || (c >= '0' && c <= '9')) jc_number();
    else jc_fail();
    jc_depth--;
}

/* Validate + pretty-print `in` into `out` (<= outmax-1 bytes + NUL). Returns -1
 * on success (out holds the re-indented JSON), else the byte offset of the first
 * syntax error. */
static int jc_run(const char *in, char *out, int outmax, int compact) {
    jc_base = jc_p = in; jc_out = out; jc_cap = outmax; jc_len = 0; jc_err = -1; jc_depth = 0; jc_compact = compact;
    jc_value(0);
    if (jc_err < 0) { jc_ws(); if (*jc_p) jc_fail(); }   /* trailing junk after the top-level value */
    out[jc_len] = 0;
    return jc_err;
}
static int json_format(const char *in, char *out, int outmax) { return jc_run(in, out, outmax, 0); }
/* Same validation, but emit COMPACT JSON (no indentation, newlines or ': ' space). */
static int json_minify(const char *in, char *out, int outmax) { return jc_run(in, out, outmax, 1); }

#endif /* JSONCORE_H */
