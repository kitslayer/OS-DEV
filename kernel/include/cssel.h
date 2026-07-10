/* The CSS simple-selector parser, extracted from browser.c so its bounds-safety over
 * untrusted <style>/inline selectors can be host-fuzzed — the codebase's extract-for-
 * testability pattern (see shgrep.h / cssprop.c / url.c / htmlentity.c). One simple
 * selector is an optional tag name followed by any run of .class / #id / [attr]
 * qualifiers; pseudo-classes/elements are stripped (approximation for a static renderer);
 * child/sibling combinators (>, +, ~) are treated like descendant (approximation that
 * captures far more rules than failing closed). Self-contained (its own cs_alnum/cs_lc)
 * so it drops into both the kernel and the host test without pulling in browser internals.
 * (M688, M1434, M1439) */
#ifndef CSSEL_H
#define CSSEL_H

typedef struct { char tag[16]; char cls[32]; char id[32]; char attr[32]; char aval[32]; char aop;
                 char dtag[16]; char dcls[32]; } sel_t;  /* a simple selector tag/.class/#id/[attr] (+ [attr=val] value in aval, op in aop: '=' '~' '^' '$' '*', 0=presence-only, M1793), + an optional descendant-ANCESTOR requirement dtag/dcls (M1434) */

static int  cs_alnum(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'); }
static int  cs_lc(int c){ return (c>='A'&&c<='Z') ? c+32 : c; }

/* Skip a pseudo-class or pseudo-element starting at s[i] (s[i] must be ':').
 * Returns the index just past the pseudo, i.e. past any '(...)' argument.
 * Bounds-safe: never reads past s[n). */
static int skip_pseudo(const char *s, int i, int n) {
    if (i >= n || s[i] != ':') return i;
    i++;                                              /* skip first ':' */
    if (i < n && s[i] == ':') i++;                   /* skip second ':' for '::' */
    while (i < n && (cs_alnum(s[i]) || s[i] == '-')) i++;  /* skip pseudo name [a-z-]+ */
    if (i < n && s[i] == '(') {                      /* skip optional argument, handling nested parens */
        int depth = 1; i++;
        while (i < n && depth > 0) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') depth--;
            i++;
        }
    }
    return i;
}

/* Parse ONE simple selector s[0..n) into tag/cls/id/attr (tag lowercased; class/id keep
 * case; attr lowercased, =value ignored). Pseudo-classes/elements (e.g. :hover, ::before,
 * :not(.x)) are stripped — a fair approximation for a static renderer. :root is mapped to
 * tag "html". Every write bounded; returns 1 if any set, 0 on an unsupported char (fail
 * closed). */
static int sel_one(const char *s, int n, char *tag, char *cls, char *id, char *attr, char *aval, char *aop) {
    tag[0]=cls[0]=id[0]=attr[0]=aval[0]=0; *aop=0;
    int i=0, k=0, ck=0;                                   /* ck: persistent cls write pos so a compound ".a.b" keeps BOTH classes (M1775) */
    while (i<n && cs_alnum(s[i]) && k<15) { tag[k++]=(char)cs_lc(s[i]); i++; }
    tag[k]=0;
    while (i<n) {
        if (s[i]=='.')      { i++; if (ck>0 && ck<31) cls[ck++]=' ';        /* space-separate multiple classes (M1775) */
                              while (i<n && (cs_alnum(s[i])||s[i]=='-'||s[i]=='_') && ck<31) cls[ck++]=s[i++]; cls[ck]=0;
                              while (i<n && (cs_alnum(s[i])||s[i]=='-'||s[i]=='_')) i++; }  /* consume any overflow chars */
        else if (s[i]=='#') { i++; k=0; while (i<n && (cs_alnum(s[i])||s[i]=='-'||s[i]=='_') && k<31) id[k++]=s[i++];  id[k]=0;
                              while (i<n && (cs_alnum(s[i])||s[i]=='-'||s[i]=='_')) i++;  }  /* consume any overflow chars */
        else if (s[i]=='[') {
            i++; k=0;
            while (i<n && s[i]!=']' && s[i]!='=' && s[i]!='~' && s[i]!='^' && s[i]!='$' && s[i]!='*' && k<31) attr[k++]=(char)cs_lc(s[i++]);
            attr[k]=0;
            char op=0;
            if (i<n && (s[i]=='~'||s[i]=='^'||s[i]=='$'||s[i]=='*')) op=(char)s[i++];   /* [a~=]/[a^=]/[a$=]/[a*=] prefix op (M1793) */
            if (i<n && s[i]=='=') { i++; if (!op) op='=';                               /* [a=value] exact */
                char q=0; if (i<n && (s[i]=='"'||s[i]=='\'')) q=(char)s[i++];            /* optional quotes around the value */
                int vk=0; while (i<n && s[i]!=']' && (q ? s[i]!=q : 1) && vk<31) aval[vk++]=s[i++]; aval[vk]=0;   /* value: case-PRESERVED (CSS attr values are case-sensitive) */
                if (q && i<n && s[i]==q) i++;
                *aop=op;
            }
            while (i<n && s[i]!=']') i++;                 /* skip any trailing (e.g. an `i`/`s` flag) up to ] */
            if (i<n && s[i]==']') i++;
        }
        else if (s[i]==':') {                         /* pseudo-class or pseudo-element: strip it */
            /* :root is a special case — treat as tag "html" */
            if (i+5<=n && s[i+1]=='r'&&s[i+2]=='o'&&s[i+3]=='o'&&s[i+4]=='t' &&
                (i+5==n || !cs_alnum(s[i+5]))) {
                if (!tag[0]) { tag[0]='h'; tag[1]='t'; tag[2]='m'; tag[3]='l'; tag[4]=0; }
                i = skip_pseudo(s, i, n);
            } else {
                i = skip_pseudo(s, i, n);             /* strip any other pseudo */
            }
        }
        else return 0;   /* unsupported combinator/char -> fail closed */
    }
    return (tag[0]||cls[0]||id[0]||attr[0]);
}

/* Find the split position of the last combinator in s[0..slen): any of whitespace, '>',
 * '+', '~' (with optional surrounding whitespace). Returns the index of the last such
 * combinator token (index of the first whitespace or combinator char), or -1 if none.
 * For "> + ~" the combinator char position is what matters; surrounding whitespace is
 * consumed by the caller. */
static int find_last_combinator(const char *s, int slen) {
    /* Walk from the right looking for the rightmost combinator boundary.
     * A combinator boundary is: whitespace OR a '>'/'+'/'~' (possibly surrounded by ws).
     * We return the position of the last such split (the position just after the target
     * simple selector starts, equivalently the index where the ancestor part ends +
     * combinator characters begin). Strategy: scan from the right for ws or >+~. */
    int last = -1, bracket = 0;
    for (int i = 0; i < slen; i++) {
        if (s[i]=='[') bracket++;                     /* M1793: a '~' inside [a~=b] is an attr operator, */
        else if (s[i]==']') { if (bracket>0) bracket--; }   /* not the general-sibling combinator — skip bracket contents */
        else if (!bracket && (s[i]==' ' || s[i]=='\t' || s[i]=='>' || s[i]=='+' || s[i]=='~')) {
            last = i;
        }
    }
    return last;
}

/* Parse a selector `s` into `o`. A single simple selector parses as before. A combinator
 * selector ("ancestor COMB target") splits on the last combinator (whitespace, >, +, or ~):
 * the target (rightmost simple selector) goes in tag/cls/id/attr, and the nearest
 * ancestor's class/tag goes in dcls/dtag as an "some ancestor must match" requirement
 * (checked against the live scope stack at match time). Child (>) and sibling (+/~)
 * combinators are treated like descendant (approximation, but captures far more rules).
 * Deeper chains keep only the nearest ancestor — an approximation. Bounded; still fails
 * closed on genuinely unparseable input. (M1434, M1439) */
static int sel_parse(const char *s, sel_t *o) {
    o->tag[0]=o->cls[0]=o->id[0]=o->attr[0]=o->aval[0]=0; o->aop=0;
    o->dtag[0]=o->dcls[0]=0;
    int slen=0; while (s[slen]) slen++;

    /* Find the last combinator position (whitespace, >, +, ~). */
    int last_comb = find_last_combinator(s, slen);
    int ts=0;
    if (last_comb >= 0) {
        /* ancestor part: s[0..ae), where ae trims trailing whitespace/combinator */
        /* target part: s[ts..slen), where ts skips leading whitespace/combinator chars */

        /* ts: step past the combinator char(s) and any surrounding whitespace */
        ts = last_comb;
        /* skip back over leading whitespace to the combinator char if needed */
        while (ts < slen && (s[ts]==' '||s[ts]=='\t'||s[ts]=='>'||s[ts]=='+'||s[ts]=='~')) ts++;

        /* ae: end of the ancestor part (trim trailing ws/combinator from [0..last_comb]) */
        int ae = last_comb;
        while (ae > 0 && (s[ae-1]==' '||s[ae-1]=='\t'||s[ae-1]=='>'||s[ae-1]=='+'||s[ae-1]=='~')) ae--;

        /* The nearest ancestor is the last simple-selector token in [0..ae).
         * Walk back from ae to find its start. */
        int as = ae, abd = 0;                        /* M1793: bracket-aware walk-back so a '~' inside a bracketed ancestor ([a~=b] > x) isn't taken as a combinator */
        while (as > 0) {
            char ac = s[as-1];
            if (ac == ']') { abd++; as--; continue; }
            if (ac == '[') { if (abd>0) abd--; as--; continue; }
            if (!abd && (ac==' '||ac=='\t'||ac=='>'||ac=='+'||ac=='~')) break;
            as--;
        }

        char et[16],ec[32],ei[32],ea[32],eav[32],eop;
        if (!(ae > as && sel_one(s+as, ae-as, et,ec,ei,ea,eav,&eop))) return 0;  /* ancestor not a simple selector -> fail closed (its attr value/op are unused) */
        int z=0; while (ec[z] && z<31) { o->dcls[z]=ec[z]; z++; } o->dcls[z]=0;    /* prefer its class */
        z=0;       while (et[z] && z<15) { o->dtag[z]=et[z]; z++; } o->dtag[z]=0;   /* else its tag */
        if (!(o->dtag[0] || o->dcls[0])) return 0;   /* id-only ancestor unsupported -> fail closed */
    }
    return sel_one(s+ts, slen-ts, o->tag,o->cls,o->id,o->attr,o->aval,&o->aop);
}

/* Does the class-attribute value v[0..vl) contain `cls` as a whole space/tab-separated
 * token? (So `.foo` matches class="a foo b" but NOT class="foobar".) The word-boundary
 * test is the one piece of CSS class matching with real logic; reached for every .class
 * selector. Bounded read-only over v[0..vl) and the NUL-terminated cls. (M690) */
static int class_has(const char *v, int vl, const char *cls) {
    int cl=0; while (cls[cl]) cl++; if (cl==0) return 0;
    for (int i=0; i+cl<=vl; i++) {
        if (i>0 && v[i-1]!=' ' && v[i-1]!='\t') continue;            /* must start at a token boundary */
        int m=0; while (m<cl && v[i+m]==cls[m]) m++;
        if (m==cl && (i+cl==vl || v[i+cl]==' ' || v[i+cl]=='\t')) return 1;   /* …and end at one */
    }
    return 0;
}

/* Every space-separated class in `cls_list` (as produced by sel_one for a compound
 * selector like ".btn.primary" -> "btn primary") must be present as a whole token in
 * v[0..vl). So ".btn.primary" matches class="primary btn extra" but NOT class="btn". (M1775) */
static int class_has_all(const char *v, int vl, const char *cls_list) {
    int i = 0;
    while (cls_list[i]) {
        while (cls_list[i] == ' ') i++;
        int cs = i; while (cls_list[i] && cls_list[i] != ' ') i++;
        if (i > cs) {
            char one[32]; int k = 0; for (int j = cs; j < i && k < 31; j++) one[k++] = cls_list[j]; one[k] = 0;
            if (!class_has(v, vl, one)) return 0;
        }
    }
    return 1;
}
/* Count the space-separated classes in a selector's cls field, for specificity
 * (each class contributes one 0-1-0). "" -> 0, "btn" -> 1, "btn primary" -> 2. (M1775) */
static int sel_class_count(const char *cls_list) {
    int n = 0, i = 0;
    while (cls_list[i]) { while (cls_list[i] == ' ') i++; if (cls_list[i]) { n++; while (cls_list[i] && cls_list[i] != ' ') i++; } }
    return n;
}
#endif
