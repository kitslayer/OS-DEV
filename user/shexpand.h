/* shexpand.h — the shell's parameter/variable expander for a command word:
 * $NAME, ${NAME}, $?, $#, $@, $*, ${#NAME}, ${VAR:-word}/${VAR:+word} default/alt,
 * ${VAR:=default}, ${VAR:off:len} slice, ${NAME#pat}/##/%/%% glob prefix/suffix
 * strip, ${VAR/pat/repl}//, ${VAR^^}/${VAR^}/${VAR,,}/${VAR,} case conversion,
 * and $((expr)) arithmetic. A
 * pass run after alias expansion and before glob/pipe/redirect. Pure apart from
 * three hooks the includer provides — vget() (variable lookup), sh_laststatus()
 * ($? value), and the shmath/shgrep helpers already in scope (sh_eval/sh_askip/
 * sh_vchar/glob_match) — so it is host-unit-tested by tests/shexpand like
 * shbrace/shgrep/shmath, and user/shell.c #includes it (run_line calls
 * expand_vars on each command line / for-list / case-word before splitting).
 *
 * Include AFTER shgrep.h (glob_match) and shmath.h (sh_eval/sh_askip/sh_vchar).
 * NOTE: keep this in sync with its host test (tests/shexpand/shexpand_test.c). */
#ifndef SHEXPAND_H
#define SHEXPAND_H

/* Provided by the includer: value of variable name[0..nl) (0 if unset), and the
 * last command's exit status for $? (already clamped to >= 0). */
static const char *vget(const char *n, int nl);
static int sh_laststatus(void);
static void vset(const char *n, int nl, const char *v);   /* assign, for ${VAR:=default} */

/* Longest prefix length L in [0..maxlen] such that glob pattern `pat` matches
 * s[0..L) (glob_match is anchored both ends, so we try each cut). -1 = no match.
 * Used by ${VAR/pat/repl} substitution to find a match at a given position. */
static int shx_glob_prefix_n(const char *pat, const char *s, int maxlen){
    char buf[260];
    if (maxlen > 259) maxlen = 259;
    for (int k = 0; k < maxlen; k++) buf[k] = s[k];
    buf[maxlen] = 0;
    int best = -1;
    for (int L = 0; L <= maxlen; L++){ char sv = buf[L]; buf[L] = 0; if (glob_match(pat, buf)) best = L; buf[L] = sv; }
    return best;
}

/* Expand $NAME / ${NAME} and $((expr)) in src into dst; returns 1 if a '$'
 * appeared (dst is then the result). */
static int expand_vars(const char *src, char *dst, int cap){
    int has=0; for (int i=0;src[i];i++) if (src[i]=='$'){ has=1; break; }
    if (!has) return 0;
    int o=0;
    for (int i=0; src[i] && o<cap-1; ){
        if (src[i]=='$' && src[i+1]=='(' && src[i+2]=='('){        /* $((expr)) arithmetic */
            const char *q = src + i + 3;
            long val = sh_eval(&q);
            sh_askip(&q); if (*q==')') q++; if (*q==')') q++;       /* consume the closing )) */
            char tmp[24]; int ti=0; int neg = val<0;
            unsigned long uv = neg ? (unsigned long)(-val) : (unsigned long)val;
            if (uv==0) tmp[ti++]='0'; while(uv){ tmp[ti++]=(char)('0'+uv%10); uv/=10; }
            if (neg) tmp[ti++]='-';
            while (ti>0 && o<cap-1) dst[o++]=tmp[--ti];
            i = (int)(q - src);
        } else if (src[i]=='$' && src[i+1]=='?'){                   /* $? -> last exit status */
            char tmp[12]; int ti=0; unsigned uv=(unsigned)sh_laststatus();
            if (uv==0) tmp[ti++]='0';
            while (uv){ tmp[ti++]=(char)('0'+uv%10); uv/=10; }
            while (ti>0 && o<cap-1) dst[o++]=tmp[--ti];
            i += 2;
        } else if (src[i]=='$' && (src[i+1]=='#' || src[i+1]=='@' || src[i+1]=='*')){ /* $# arg count, $@/$* all args (set on a function call) */
            const char *v = vget((src[i+1]=='*') ? "@" : src+i+1, 1);   /* $* == $@ in this shell's single-string args model; before M1750 $* fell through to $NAME leaving a bare * -> a cwd glob */
            if (v) for (int k=0; v[k] && o<cap-1; k++) dst[o++]=v[k];
            i += 2;
        } else if (src[i]=='$'){                                    /* $NAME / ${NAME} / ${NAME:-w} / ${NAME:+w} / ${#NAME} / ${NAME#pat} / ${NAME%pat} */
            int br=(src[i+1]=='{');
            if (br && src[i+2]=='#') {                              /* ${#NAME} = length of NAME; ${#} = arg count */
                int ns=i+3, ne=ns; while (src[ne] && sh_vchar(src[ne])) ne++;
                if (ne > ns) {
                    const char *v=vget(src+ns, ne-ns);
                    int len=0; if (v) while (v[len]) len++;
                    char tmp[12]; int ti=0; if (!len) tmp[ti++]='0'; while (len) { tmp[ti++]=(char)('0'+len%10); len/=10; }
                    while (ti>0 && o<cap-1) dst[o++]=tmp[--ti];
                } else {
                    const char *v=vget("#",1);
                    if (v) for (int k=0; v[k] && o<cap-1; k++) dst[o++]=v[k];
                }
                i = (src[ne]=='}') ? ne+1 : ne;
            } else if (br && src[i+2]=='!') {                       /* ${!NAME} indirect: value of the variable whose name is NAME's value (M1826) */
                int ns=i+3, ne=ns; while (src[ne] && sh_vchar(src[ne])) ne++;
                if (ne > ns) {
                    const char *iname=vget(src+ns, ne-ns);          /* NAME's value = the target variable's name */
                    if (iname && iname[0]) {
                        int il=0; while (iname[il]) il++;
                        const char *v=vget(iname, il);
                        if (v) for (int k=0; v[k] && o<cap-1; k++) dst[o++]=v[k];
                    }
                }
                i = (src[ne]=='}') ? ne+1 : ne;
            } else {
                int s=i+1+br, e=s; while (src[e] && sh_vchar(src[e])) e++;
                const char *v=(e>s)?vget(src+s,e-s):0;
                if (br && ((src[e]==':' && (src[e+1]=='-' || src[e+1]=='+')) || src[e]=='-' || src[e]=='+')) {   /* ${VAR:-word}/${VAR-word} default, ${VAR:+word}/${VAR+word} alt (literal word) */
                    int colon=(src[e]==':'), plus=(src[e+colon]=='+');
                    int set = colon ? (v && v[0]) : (v != 0);   /* colon form: set-AND-nonempty; no-colon form: merely set (empty counts) */
                    int ws=e+colon+1, we=ws; while (src[we] && src[we]!='}') we++;
                    const char *w=src+ws; int wl=we-ws;
                    if (plus) { if (set) { for (int k=0; k<wl && o<cap-1; k++) dst[o++]=w[k]; } }
                    else if (set) { for (int k=0; v[k] && o<cap-1; k++) dst[o++]=v[k]; }
                    else { for (int k=0; k<wl && o<cap-1; k++) dst[o++]=w[k]; }
                    i = (src[we]=='}') ? we+1 : we;
                } else if (br && ((src[e]==':' && src[e+1]=='=') || src[e]=='=') && e>s) {   /* ${VAR:=default}/${VAR=default}: if unset(/empty), assign default AND expand to it */
                    int colon=(src[e]==':');
                    int ws=e+colon+1, we=ws; while (src[we] && src[we]!='}') we++;
                    int set = colon ? (v && v[0]) : (v != 0);
                    if (set) { for (int k=0; v[k] && o<cap-1; k++) dst[o++]=v[k]; }
                    else {
                        char wtmp[128]; int wl=0; for (int k=ws; k<we && wl<127; k++) wtmp[wl++]=src[k]; wtmp[wl]=0;
                        vset(src+s, e-s, wtmp);              /* the assignment side effect */
                        for (int k=0; k<wl && o<cap-1; k++) dst[o++]=wtmp[k];
                    }
                    i = (src[we]=='}') ? we+1 : we;
                } else if (br && src[e]==':' && e>s &&
                           ((src[e+1]>='0'&&src[e+1]<='9') || src[e+1]==' ')) {   /* ${VAR:off} / ${VAR:off:len} substring (bash slice) */
                    int p=e+1; while (src[p]==' ') p++;
                    long off=0; int oneg=0; if (src[p]=='-'){oneg=1;p++;} else if (src[p]=='+') p++;
                    while (src[p]>='0'&&src[p]<='9'){ off=off*10+(src[p]-'0'); p++; } if (oneg) off=-off;
                    int hasLen=0; long len=0; int lneg=0;
                    while (src[p]==' ') p++;
                    if (src[p]==':'){ hasLen=1; p++; while (src[p]==' ') p++; if (src[p]=='-'){lneg=1;p++;} else if (src[p]=='+') p++;
                        while (src[p]>='0'&&src[p]<='9'){ len=len*10+(src[p]-'0'); p++; } if (lneg) len=-len; }
                    while (src[p] && src[p]!='}') p++;                    /* tolerate trailing junk up to '}' */
                    char vb[260]; int vl=0; if (v) for (int k=0; v[k] && vl<259; k++) vb[vl++]=v[k]; vb[vl]=0;
                    long n=vl, start=off, end;
                    if (start<0){ start=n+start; if (start<0) start=n; } else if (start>n) start=n;   /* offset before the start -> empty (bash), not clamp-to-whole-string */
                    if (!hasLen) end=n;
                    else if (len>=0){ end=start+len; if (end>n) end=n; }
                    else end=n+len;                                      /* negative length: end measured from the string's end */
                    if (end<start) end=start;
                    for (long k=start; k<end && o<cap-1; k++) dst[o++]=vb[k];
                    i = (src[p]=='}') ? (int)(p+1) : (int)p;
                } else if (br && (src[e]=='#' || src[e]=='%') && e>s) {   /* ${NAME#pat}/##/%/%% : strip a glob prefix/suffix */
                    char op=src[e]; int lng=(src[e+1]==op); int ps=e+1+lng, pe=ps;
                    while (src[pe] && src[pe]!='}') pe++;
                    char pat[80]; int pl=0; for (int k=ps; k<pe && pl<79; k++) pat[pl++]=src[k]; pat[pl]=0;
                    char vb[260]; int vl=0; if (v) for (int k=0; v[k] && vl<259; k++) vb[vl++]=v[k]; vb[vl]=0;
                    int keepStart=0, keepLen=vl;
                    if (op=='%') {                                  /* strip the shortest (%) or longest (%%) matching suffix */
                        int best=-1;
                        for (int sl=0; sl<=vl; sl++) if (glob_match(pat, vb+vl-sl)) { best=sl; if (!lng) break; }
                        if (best>=0) keepLen=vl-best;
                    } else {                                        /* strip the shortest (#) or longest (##) matching prefix */
                        int best=-1;
                        for (int q=0; q<=vl; q++) { char sv=vb[q]; vb[q]=0; int m=glob_match(pat, vb); vb[q]=sv; if (m) { best=q; if (!lng) break; } }
                        if (best>=0) { keepStart=best; keepLen=vl-best; }
                    }
                    for (int k=0; k<keepLen && o<cap-1; k++) dst[o++]=vb[keepStart+k];
                    i = (src[pe]=='}') ? pe+1 : pe;
                } else if (br && src[e]=='/' && e>s) {           /* ${VAR/pat/repl} first / ${VAR//pat/repl} all: glob substitution */
                    int global=(src[e+1]=='/'), ps=e+1+global;
                    char pat[80]; int pl=0;                       /* pattern: to an unescaped '/' or '}', \x -> literal x */
                    while (src[ps] && src[ps]!='}' && !(src[ps]=='/')) {
                        if (src[ps]=='\\' && src[ps+1]) { if (pl<79) pat[pl++]=src[ps+1]; ps+=2; }
                        else { if (pl<79) pat[pl++]=src[ps]; ps++; }
                    }
                    pat[pl]=0;
                    char rep[80]; int rl=0; int rs=ps;
                    if (src[ps]=='/') { rs=ps+1; int rp=rs;       /* replacement: to '}', \x -> literal x */
                        while (src[rp] && src[rp]!='}') {
                            if (src[rp]=='\\' && src[rp+1]) { if (rl<79) rep[rl++]=src[rp+1]; rp+=2; }
                            else { if (rl<79) rep[rl++]=src[rp]; rp++; }
                        }
                        ps=rp;                                    /* ps now at the closing '}' (or end) */
                    }
                    rep[rl]=0;
                    char vb[260]; int vl=0; if (v) for (int k=0; v[k] && vl<259; k++) vb[vl++]=v[k]; vb[vl]=0;
                    if (pl==0) {                                  /* empty pattern: no-op, emit value verbatim */
                        for (int k=0; k<vl && o<cap-1; k++) dst[o++]=vb[k];
                    } else {
                        int p=0, did=0;
                        while (p<vl && o<cap-1) {
                            int ml = (global || !did) ? shx_glob_prefix_n(pat, vb+p, vl-p) : -1;
                            if (ml>=0) {                          /* match at p: emit replacement */
                                for (int k=0; k<rl && o<cap-1; k++) dst[o++]=rep[k];
                                did=1;
                                if (ml==0) { if (o<cap-1) dst[o++]=vb[p]; p++; }   /* zero-length match: keep the char, avoid a stall */
                                else p+=ml;
                            } else { dst[o++]=vb[p]; p++; }
                        }
                    }
                    i = (src[ps]=='}') ? ps+1 : ps;
                } else if (br && (src[e]=='^' || src[e]==',') && e>s) {   /* ${VAR^^}/${VAR^} upper, ${VAR,,}/${VAR,} lower (M1821) */
                    char op=src[e]; int all=(src[e+1]==op);
                    int ps=e+1+all, pe=ps; while (src[pe] && src[pe]!='}') pe++;   /* optional glob pattern ps..pe: only matching chars convert (bash) */
                    char pat[80]; int pl=0; for (int k=ps; k<pe && pl<79; k++) pat[pl++]=src[k]; pat[pl]=0;
                    if (v) {
                        int first=1;
                        for (int k=0; v[k] && o<cap-1; k++) {
                            char c=v[k];
                            int apply = all || first;                     /* ^^/,, : every char;  ^/, : first char only */
                            if (apply && pl>0) { char one[2]={c,0}; apply = glob_match(pat, one); }   /* gate on the pattern (no pattern -> match all) */
                            if (apply) {
                                if (op=='^' && c>='a' && c<='z') c-=32;
                                else if (op==',' && c>='A' && c<='Z') c+=32;
                            }
                            dst[o++]=c; first=0;
                        }
                    }
                    i = (src[pe]=='}') ? pe+1 : pe;
                } else {
                    if (v) for (int k=0; v[k] && o<cap-1; k++) dst[o++]=v[k];
                    i = e + ((br && src[e]=='}')?1:0);
                }
            }
        } else dst[o++]=src[i++];
    }
    dst[o]=0; return 1;
}

#endif /* SHEXPAND_H */
