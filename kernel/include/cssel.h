/* The CSS simple-selector parser, extracted from browser.c so its bounds-safety over
 * untrusted <style>/inline selectors can be host-fuzzed — the codebase's extract-for-
 * testability pattern (see shgrep.h / cssprop.c / url.c / htmlentity.c). One simple
 * selector is an optional tag name followed by any run of .class / #id / [attr]
 * qualifiers; the parser fails CLOSED (returns 0 = match nothing) on any unsupported
 * combinator or character. Self-contained (its own cs_alnum/cs_lc) so it drops into both
 * the kernel and the host test without pulling in browser internals. (M688) */
#ifndef CSSEL_H
#define CSSEL_H

typedef struct { char tag[16]; char cls[32]; char id[32]; char attr[32]; } sel_t;  /* one simple selector: tag/.class/#id/[attr] */

static int  cs_alnum(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'); }
static int  cs_lc(int c){ return (c>='A'&&c<='Z') ? c+32 : c; }

/* Parse one simple selector `s` into `o`: tag lowercased; class/id keep case (allowing
 * -/_); attr lowercased with any `=value` ignored. EVERY write is bounded to the fixed
 * fields (tag[16]/cls[32]/id[32]/attr[32], NUL-terminated within), so a long or
 * adversarial selector truncates instead of overflowing. Returns 1 if any component set. */
static int sel_parse(const char *s, sel_t *o) {
    o->tag[0]=o->cls[0]=o->id[0]=o->attr[0]=0;
    int i=0, k=0;
    while (s[i] && cs_alnum(s[i]) && k<15) { o->tag[k++]=(char)cs_lc(s[i]); i++; }   /* leading tag name (lowercased) */
    o->tag[k]=0;
    while (s[i]) {
        if (s[i]=='.')      { i++; k=0; while (s[i] && (cs_alnum(s[i])||s[i]=='-'||s[i]=='_') && k<31) o->cls[k++]=s[i++]; o->cls[k]=0; }
        else if (s[i]=='#') { i++; k=0; while (s[i] && (cs_alnum(s[i])||s[i]=='-'||s[i]=='_') && k<31) o->id[k++]=s[i++];  o->id[k]=0;  }
        else if (s[i]=='[') {                              /* [attr] presence; any =value is ignored */
            i++; k=0;
            while (s[i] && s[i]!=']' && s[i]!='=' && k<31) o->attr[k++]=(char)cs_lc(s[i++]);
            o->attr[k]=0;
            while (s[i] && s[i]!=']') i++;
            if (s[i]==']') i++;
        }
        else return 0;   /* an unsupported combinator/char -> fail closed (no match) */
    }
    return (o->tag[0]||o->cls[0]||o->id[0]||o->attr[0]);
}
#endif
