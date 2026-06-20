/* shsplit.h — the shell's statement splitter: given a logical input line, find
 * the next top-level ';' that ends a statement. It skips ';' inside command/
 * arithmetic substitution ($( ) and $(( )) ) and inside control constructs
 * (if…fi / while…done / for…done), so a construct's own ';'s aren't break points
 * and constructs can follow a ';' or live in a function body. Pure (no syscalls),
 * so it's host-unit-tested by tests/shsplit; user/shell.c #includes it and
 * run_input_line walks segments with it. */
#ifndef SHSPLIT_H
#define SHSPLIT_H

/* Is keyword `kw` a whole word at p? (the caller has already established that p is
 * at a command position.) A word ends at a space, a ';', or end-of-string. */
static int word_at(const char *p, const char *kw) {
    int i = 0; while (kw[i] && p[i] == kw[i]) i++;
    if (kw[i]) return 0;
    char a = p[i];
    return a == 0 || a == ' ' || a == ';';
}

/* Offset into `seg` of the next top-level ';' statement separator, or the offset
 * of the terminating '\0' if there isn't one. `cd` tracks control-construct depth;
 * `atcmd` marks a command position, where a leading keyword opens (if/for/while)
 * or closes (fi/done) a construct, and then/do/else re-arm the next position. */
static int sh_next_sep(const char *seg) {
    const char *semi = seg; int pd = 0, cd = 0, atcmd = 1;
    while (*semi) {
        if (semi[0] == '$' && semi[1] == '(') { pd++; semi += 2; atcmd = 0; continue; }
        if (pd > 0) { if (*semi == '(') pd++; else if (*semi == ')') pd--; semi++; continue; }
        if (*semi == ';') { if (cd == 0) break; semi++; atcmd = 1; continue; }
        if (*semi == ' ') { semi++; continue; }
        if (atcmd) {
            if (word_at(semi, "if") || word_at(semi, "for") || word_at(semi, "while")) cd++;
            else if ((word_at(semi, "fi") || word_at(semi, "done")) && cd > 0) cd--;
        }
        int nextcmd = word_at(semi, "then") || word_at(semi, "do") || word_at(semi, "else");
        while (*semi && *semi != ' ' && *semi != ';') semi++;   /* skip this whole token */
        atcmd = nextcmd;
    }
    return (int)(semi - seg);
}

#endif /* SHSPLIT_H */
