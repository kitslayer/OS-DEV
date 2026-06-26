/* The CSS simple-selector parser, extracted from browser.c so its bounds-safety over
 * untrusted <style>/inline selectors can be host-fuzzed — the codebase's extract-for-
 * testability pattern (see shgrep.h / cssprop.c / url.c / htmlentity.c). One simple
 * selector is an optional tag name followed by any run of .class / #id / [attr]
 * qualifiers; the parser fails CLOSED (returns 0 = match nothing) on any unsupported
 * combinator or character. Self-contained (its own cs_alnum/cs_lc) so it drops into both
 * the kernel and the host test without pulling in browser internals. (M688) */
#ifndef CSSEL_H
#define CSSEL_H

typedef struct { char tag[16]; char cls[32]; char id[32]; char attr[32];
                 char dtag[16]; char dcls[32]; } sel_t;  /* a simple selector tag/.class/#id/[attr], + an optional descendant-ANCESTOR requirement dtag/dcls (M1434) */

static int  cs_alnum(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'); }
static int  cs_lc(int c){ return (c>='A'&&c<='Z') ? c+32 : c; }

/* Parse ONE simple selector s[0..n) into tag/cls/id/attr (tag lowercased; class/id keep
 * case; attr lowercased, =value ignored). Every write bounded; returns 1 if any set, 0 on
 * an unsupported char (fail closed). */
static int sel_one(const char *s, int n, char *tag, char *cls, char *id, char *attr) {
    tag[0]=cls[0]=id[0]=attr[0]=0;
    int i=0, k=0;
    while (i<n && cs_alnum(s[i]) && k<15) { tag[k++]=(char)cs_lc(s[i]); i++; }
    tag[k]=0;
    while (i<n) {
        if (s[i]=='.')      { i++; k=0; while (i<n && (cs_alnum(s[i])||s[i]=='-'||s[i]=='_') && k<31) cls[k++]=s[i++]; cls[k]=0; }
        else if (s[i]=='#') { i++; k=0; while (i<n && (cs_alnum(s[i])||s[i]=='-'||s[i]=='_') && k<31) id[k++]=s[i++];  id[k]=0;  }
        else if (s[i]=='[') {
            i++; k=0;
            while (i<n && s[i]!=']' && s[i]!='=' && k<31) attr[k++]=(char)cs_lc(s[i++]);
            attr[k]=0;
            while (i<n && s[i]!=']') i++;
            if (i<n && s[i]==']') i++;
        }
        else return 0;   /* unsupported combinator/char -> fail closed */
    }
    return (tag[0]||cls[0]||id[0]||attr[0]);
}

/* Parse a selector `s` into `o`. A single simple selector parses as before. A DESCENDANT
 * selector ("ancestor … target") splits on the last whitespace: the target (rightmost
 * simple selector) goes in tag/cls/id/attr, and the nearest ancestor's class/tag goes in
 * dcls/dtag as an "some ancestor must match" requirement (checked against the live scope
 * stack at match time). Deeper chains keep only the nearest ancestor — an approximation,
 * but it lets `.nav-links a`, `.entry-links a`, etc. apply instead of being dropped.
 * Still bounded; combinators other than descendant (>, +, ~) still fail closed. */
static int sel_parse(const char *s, sel_t *o) {
    o->tag[0]=o->cls[0]=o->id[0]=o->attr[0]=0;
    o->dtag[0]=o->dcls[0]=0;
    int slen=0; while (s[slen]) slen++;
    int last_sp=-1; for (int i=0;i<slen;i++) if (s[i]==' '||s[i]=='\t') last_sp=i;
    int ts=0;
    if (last_sp>=0) {                                      /* descendant combinator */
        int ae=last_sp; while (ae>0 && (s[ae-1]==' '||s[ae-1]=='\t')) ae--;   /* trim trailing ws of the ancestor part */
        int as=ae;      while (as>0 && s[as-1]!=' ' && s[as-1]!='\t') as--;   /* nearest ancestor = the last token */
        char et[16],ec[32],ei[32],ea[32];
        if (!(ae>as && sel_one(s+as, ae-as, et,ec,ei,ea))) return 0;          /* ancestor not a simple selector (e.g. `div > p`) -> fail closed */
        int z=0; while (ec[z] && z<31) { o->dcls[z]=ec[z]; z++; } o->dcls[z]=0;    /* prefer its class */
        z=0;       while (et[z] && z<15) { o->dtag[z]=et[z]; z++; } o->dtag[z]=0;   /* else its tag */
        if (!(o->dtag[0] || o->dcls[0])) return 0;                            /* id-only ancestor unsupported -> fail closed */
        ts=last_sp+1; while (ts<slen && (s[ts]==' '||s[ts]=='\t')) ts++;
    }
    return sel_one(s+ts, slen-ts, o->tag,o->cls,o->id,o->attr);
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
#endif
