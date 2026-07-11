/* shquote.h — POSIX-style shell quoting: "double" and 'single' quotes.
 *
 * sh_quote_pass() rewrites a command string IN PLACE: it removes the quote
 * characters and PROTECTS the special bytes that were inside them by setting
 * bit 7 (shell input is 7-bit ASCII, so the high bit is free as a sentinel).
 * The whole existing expansion/splitting pipeline then treats those bytes as
 * ordinary content — every splitter/glob/expander scans for the *raw* ASCII
 * special, and a high-bit byte is not it — so a quoted space won't word-split,
 * a quoted ; | & < > won't break the line, a quoted * won't glob, and (inside
 * single quotes) a $ won't expand. Each builtin calls sh_unprot_buf() on a token
 * once it has been split out, restoring the real bytes for use/printing.
 *
 *   double quotes:  protect space ; | & < > * ?  (leave $ ( ) { } ` for expansion)
 *   single quotes:  protect those PLUS ( ) { } , $ `  (fully literal — no expansion)
 *   \" \\ \$ \`     inside double quotes -> the escaped char, protected (literal)
 *
 * Pure (no syscalls), so it's host-fuzzed by tests/shquote like shsplit/shbrace.
 * user/shell.c #includes it; run_line() applies sh_quote_pass() to the command
 * before any expansion, and the builtins sh_unprot_buf() their argument tokens. */
#ifndef SHQUOTE_H
#define SHQUOTE_H

#define SH_PROT(c)    ((char)((unsigned char)(c) | 0x80u))
#define SH_ISPROT(c)  ((unsigned char)(c) & 0x80u)
#define SH_UNPROT(c)  ((char)((unsigned char)(c) & 0x7Fu))

/* Restore any protected bytes (bit 7 set) in a NUL-terminated token to their real
 * ASCII. Safe because shell input is 7-bit, so a high-bit byte is always a sentinel. */
static void sh_unprot_buf(char *s) {
    for (; *s; s++) if (SH_ISPROT(*s)) *s = SH_UNPROT(*s);
}

/* Bytes a later pipeline stage would act on if left literal (both quote kinds).
 * '~' is here so a quoted "~" stays literal rather than being tilde-expanded (M1806). */
static int shq_special_both(char c) {
    return c==' '||c=='\t'||c==';'||c=='|'||c=='&'||c=='<'||c=='>'||c=='*'||c=='?'||c=='~';
}
/* Additionally protected inside single quotes (single quotes suppress ALL of it). */
static int shq_special_single(char c) {
    return c=='('||c==')'||c=='{'||c=='}'||c==','||c=='$'||c=='`';
}

/* Strip quotes + protect their contents, in place. Unbalanced quote -> the open
 * span implicitly closes at end-of-line (never errors/crashes). */
static void sh_quote_pass(char *line) {
    char *r = line, *w = line; int q = 0;          /* q: 0 (none), '"' or '\'' */
    while (*r) {
        char c = *r;
        /* $(...) command/arith substitution: copy it VERBATIM (depth-tracked), even
         * inside double quotes — its content is a separate command whose own quoting
         * is processed when it runs, so we must NOT protect its spaces. (Single quotes
         * suppress substitution, so there $ is just protected below.) */
        if (q != '\'' && c == '$' && r[1] == '(') {
            *w++ = *r++; *w++ = *r++;              /* "$(" */
            int depth = 1;
            while (*r && depth) { if (*r == '(') depth++; else if (*r == ')') depth--; *w++ = *r++; }
            continue;
        }
        if (!q) {
            if (c == '"' || c == '\'') { q = c; r++; continue; }   /* open quote: drop it */
            *w++ = c; r++; continue;                                /* ordinary char */
        }
        if (c == q) { q = 0; r++; continue; }                       /* matching close: drop it */
        if (q == '"' && c == '\\' && (r[1]=='"' || r[1]=='\\' || r[1]=='$' || r[1]=='`')) {
            *w++ = SH_PROT(r[1]); r += 2; continue;                 /* \" \\ \$ \` -> literal char */
        }
        int other_quote = (c == '"' || c == '\'');   /* the OTHER quote kind inside this one (c != q here): keep it
                                                       * literal AND protected so a nested re-parse can't re-open it */
        int prot = shq_special_both(c) || other_quote || (q == '\'' && shq_special_single(c));
        *w++ = prot ? SH_PROT(c) : c;
        r++;
    }
    *w = 0;
}

#endif /* SHQUOTE_H */
