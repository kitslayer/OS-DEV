/*
 * js.c — a small from-scratch JavaScript interpreter (tree-walking).
 *
 * Supports a useful core of the language: var/let/const, integer numbers,
 * strings, booleans, null/undefined; the usual operators (+ - * / %, comparisons,
 * && || !, ?:, ++/--, typeof); if/else, while, for, blocks, break/continue;
 * function declarations + expressions with lexical closures and recursion;
 * array and object literals with member/index access, .length and a few methods;
 * and the builtins print()/console.log() plus a small Math.
 *
 * Deliberate simplifications (this is an OS kernel with no FPU and tiny stacks):
 *  - Number is a 64-bit INTEGER (the kernel is built -mgeneral-regs-only, no
 *    floating point). So 7/2 === 3. Float support would need soft-float.
 *  - Memory is an arena that is reset wholesale after each top-level run (no GC):
 *    one script runs, prints, and the arena is recycled. Bounded by JS_ARENA.
 *  - Recursion (parser + evaluator) is depth-limited to stay within the stack.
 *
 * The file compiles both freestanding in the kernel and, with -DJS_HOSTTEST, as
 * a standalone host program for testing (see the main() at the bottom).
 */
#ifdef JS_HOSTTEST
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#else
#include "string.h"
#include "rtc.h"
#include <stdint.h>
#include <stddef.h>
#endif

/* ---- output sink: all print() output is appended here ---- */
static char  *g_out;
static int    g_out_cap, g_out_len;
static void out_str(const char *s) {
    while (*s && g_out_len < g_out_cap - 1) g_out[g_out_len++] = *s++;
    if (g_out_cap) g_out[g_out_len] = 0;
}

/* ---- arena allocator (no free; reset per run) ---- */
/* Holds the whole parsed AST + the global objects + every value a run allocates,
 * all at once (reset only between runs). 2 MB so large scripts — the kitchen-sink
 * regression suite, or a page that defines several classes — have eval headroom
 * above the parsed AST; the buffer is static BSS, cheap on the kernel's RAM. */
#define JS_ARENA   (2048 * 1024)
#ifdef JS_HOSTTEST
static char g_arena_buf[JS_ARENA];
#else
static char g_arena_buf[JS_ARENA];   /* BSS */
#endif
static int  g_arena_off;
static int  g_oom;
/* n is a 64-bit signed size: callers pass sizeof(T)*count which is size_t, so a
 * count that would overflow a 32-bit size arrives here as a huge value and is
 * rejected (rather than silently truncating to a small/zero size -> OOB). */
static void *aalloc(long n) {
    if (n < 0) { g_oom = 1; return 0; }
    n = (n + 7) & ~7;
    if (n > (long)(JS_ARENA - g_arena_off)) { g_oom = 1; return 0; }   /* incl. n > JS_ARENA */
    void *p = g_arena_buf + g_arena_off; g_arena_off += (int)n;
    return p;
}
/* intern a (possibly non-terminated) name into a stable, NUL-terminated arena
 * string. Done once at parse time so eval never re-allocates for identifiers. */
static const char *intern(const char *s, int len) {
    char *p = aalloc(len + 1); if (!p) return "";
    if (len > 0) memcpy(p, s, len);    /* guard memcpy(_, NULL, 0) for an OOM'd token (UB) */
    p[len] = 0; return p;
}

/* ---- runtime error handling ---- */
/* g_err unwinds eval like an exception; a try/catch clears it. g_threw marks that
 * the in-flight error is an explicit `throw` (carrying g_throwval) vs a built-in
 * runtime error (caught as its message string). g_oom is NOT catchable. */
static int  g_err;
static char g_errmsg[128];
static int  g_threw;
static void rt_err(const char *m) {
    if (!g_err) { g_err = 1; int i = 0; while (m[i] && i < 127) { g_errmsg[i] = m[i]; i++; } g_errmsg[i] = 0; }
}

/* =========================== lexer =========================== */
enum { T_EOF, T_NUM, T_STR, T_IDENT, T_PUNC, T_KW, T_TEMPLATE, T_REGEX };
typedef struct { int type; int64_t num; const char *s; int len; } token;

static const char *kw[] = { "var","let","const","function","return","if","else",
    "while","for","true","false","null","undefined","break","continue","typeof",
    "switch","case","default","do","try","catch","finally","throw","this","new",
    "class","extends","super",0 };

typedef struct {
    const char *src; int pos, len;
    token cur, peeked; int has_peek;
    token last;   /* last token lex_next produced — used to decide `/` = regex vs division */
} lexer;

static int tok_is(token t, const char *p);   /* fwd: used by the regex/division decision in lex_next_raw */
static int is_id_start(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'; }
static int is_id(int c){ return is_id_start(c)||(c>='0'&&c<='9'); }
static int is_digit(int c){ return c>='0'&&c<='9'; }

static token lex_next_raw(lexer *L) {
    token t; t.type = T_EOF; t.s = 0; t.len = 0; t.num = 0;
    const char *s = L->src;
    /* skip whitespace + comments */
    for (;;) {
        while (L->pos < L->len && (s[L->pos]==' '||s[L->pos]=='\t'||s[L->pos]=='\n'||s[L->pos]=='\r')) L->pos++;
        if (L->pos+1 < L->len && s[L->pos]=='/' && s[L->pos+1]=='/') { while (L->pos<L->len && s[L->pos]!='\n') L->pos++; continue; }
        if (L->pos+1 < L->len && s[L->pos]=='/' && s[L->pos+1]=='*') { L->pos+=2; while (L->pos+1<L->len && !(s[L->pos]=='*'&&s[L->pos+1]=='/')) L->pos++; L->pos+=2; continue; }
        break;
    }
    if (L->pos >= L->len) return t;
    int c = s[L->pos];
    /* number (integer) */
    if (is_digit(c)) {
        int64_t v = 0; int start = L->pos;
        if (c=='0' && L->pos+1<L->len && (s[L->pos+1]=='x'||s[L->pos+1]=='X')) {
            L->pos += 2;
            while (L->pos<L->len) { int d=s[L->pos]; int h;
                if (d>='0'&&d<='9') h=d-'0'; else if (d>='a'&&d<='f') h=d-'a'+10; else if (d>='A'&&d<='F') h=d-'A'+10; else break;
                v = v*16 + h; L->pos++; }
        } else {
            while (L->pos<L->len && is_digit(s[L->pos])) { v = v*10 + (s[L->pos]-'0'); L->pos++; }
            /* skip a fractional part if present (we truncate to int) */
            if (L->pos<L->len && s[L->pos]=='.') { L->pos++; while (L->pos<L->len && is_digit(s[L->pos])) L->pos++; }
        }
        (void)start; t.type=T_NUM; t.num=v; return t;
    }
    /* string */
    if (c=='"' || c=='\'') {
        int q = c; L->pos++;
        char *buf = aalloc(L->len); int n = 0;       /* generous; arena */
        while (L->pos<L->len && s[L->pos]!=q) {
            int ch = s[L->pos++];
            if (ch=='\\' && L->pos<L->len) {
                int e = s[L->pos++];
                ch = e=='n'?'\n': e=='t'?'\t': e=='r'?'\r': e=='\\'?'\\': e=='\''?'\'': e=='"'?'"': e=='0'?0: e;
            }
            if (buf) buf[n++] = (char)ch;
        }
        if (L->pos<L->len) L->pos++;                 /* closing quote */
        if (buf) buf[n]=0;
        t.type=T_STR; t.s=buf; t.len=n; return t;
    }
    /* template literal `...${expr}...` — capture the raw inner text (balancing
     * ${ } so a `}` inside a substitution doesn't end it); the parser splits it. */
    if (c=='`') {
        int start = L->pos+1, i = start, depth = 0;
        while (i < L->len) {
            char ch = s[i];
            if (ch=='\\') { i += 2; continue; }
            if (depth==0 && ch=='`') break;
            if (ch=='$' && i+1<L->len && s[i+1]=='{') { depth++; i += 2; continue; }
            if (depth>0 && ch=='{') depth++;
            else if (depth>0 && ch=='}') depth--;
            i++;
        }
        t.type=T_TEMPLATE; t.s=s+start; t.len=i-start; L->pos = (i<L->len)? i+1 : i;
        return t;
    }
    /* identifier / keyword */
    if (is_id_start(c)) {
        int start = L->pos; while (L->pos<L->len && is_id(s[L->pos])) L->pos++;
        t.s = s+start; t.len = L->pos-start; t.type = T_IDENT;
        for (int i=0; kw[i]; i++) { int kl=(int)strlen(kw[i]); if (kl==t.len && memcmp(kw[i],t.s,kl)==0) { t.type=T_KW; break; } }
        return t;
    }
    /* regex literal /pattern/flags — `/` starts one unless the previous token ends an
     * operand (then it's division). The pattern bypasses the parser's depth guard, so
     * the regex compiler caps its own recursion (M179). */
    if (c=='/') {
        token p=L->last; int div_ctx=0;
        if (p.type==T_NUM||p.type==T_STR||p.type==T_TEMPLATE||p.type==T_IDENT||p.type==T_REGEX) div_ctx=1;
        else if (p.type==T_PUNC && (tok_is(p,")")||tok_is(p,"]")||tok_is(p,"++")||tok_is(p,"--"))) div_ctx=1;   /* postfix x++ / 2 is division */
        else if (p.type==T_KW && (tok_is(p,"this")||tok_is(p,"true")||tok_is(p,"false")||tok_is(p,"null")||tok_is(p,"undefined")||tok_is(p,"super"))) div_ctx=1;
        if (!div_ctx) {
            L->pos++;                                   /* opening / */
            int pstart=L->pos, inclass=0;
            while (L->pos<L->len) { int ch=s[L->pos];
                if (ch=='\\') { L->pos+=2; continue; }
                if (ch=='\n') break;
                if (ch=='[') inclass=1; else if (ch==']') inclass=0; else if (ch=='/' && !inclass) break;
                L->pos++; }
            int plen=L->pos-pstart;
            if (L->pos<L->len && s[L->pos]=='/') L->pos++;   /* closing / */
            int64_t fb=0; while (L->pos<L->len && is_id(s[L->pos])) { char f=s[L->pos]; if(f=='g')fb|=1; else if(f=='i')fb|=2; L->pos++; }
            t.type=T_REGEX; t.s=s+pstart; t.len=plen; t.num=fb; return t;
        }
    }
    /* punctuation / multi-char operators */
    static const char *ops[] = { "===","!==","<<=",">>=","...","==","!=","<=",">=",
        "&&","||","??","?.","++","--","+=","-=","*=","/=","%=","<<",">>","=>",0 };
    for (int i=0; ops[i]; i++) { int ol=(int)strlen(ops[i]); if (L->pos+ol<=L->len && memcmp(ops[i],s+L->pos,ol)==0) { t.type=T_PUNC; t.s=s+L->pos; t.len=ol; L->pos+=ol; return t; } }
    t.type=T_PUNC; t.s=s+L->pos; t.len=1; L->pos++; return t;
}

static token lex_next(lexer *L){ token t=lex_next_raw(L); L->last=t; return t; }   /* track last token for the `/` regex/division decision */
static token peek(lexer *L){ if (!L->has_peek){ L->peeked=lex_next(L); L->has_peek=1; } return L->peeked; }
static token advance(lexer *L){ if (L->has_peek){ L->has_peek=0; return L->peeked; } return lex_next(L); }
/* save/restore lexer position for bounded lookahead (arrow-function detection) */
typedef struct { int pos, has_peek; token peeked, last; } lexsave;
static lexsave lex_save(lexer *L){ lexsave s; s.pos=L->pos; s.has_peek=L->has_peek; s.peeked=L->peeked; s.last=L->last; return s; }
static void lex_restore(lexer *L, lexsave s){ L->pos=s.pos; L->has_peek=s.has_peek; L->peeked=s.peeked; L->last=s.last; }
static int tok_is(token t, const char *p){ int l=(int)strlen(p); return t.len==l && t.s && memcmp(t.s,p,l)==0; }
static int peek_punc(lexer *L, const char *p){ token t=peek(L); return t.type==T_PUNC && tok_is(t,p); }
static int peek_kw(lexer *L, const char *p){ token t=peek(L); return t.type==T_KW && tok_is(t,p); }
static void skip_semi(lexer *L){ while (peek_punc(L,";")) advance(L); }   /* ; is an optional terminator */

/* =========================== AST =========================== */
enum { N_NUM, N_STR, N_BOOL, N_NULL, N_UNDEF, N_IDENT, N_ARRAY, N_OBJECT,
       N_FUNC, N_CALL, N_MEMBER, N_INDEX, N_UNARY, N_UPDATE, N_BINARY, N_LOGICAL,
       N_ASSIGN, N_COND, N_VAR, N_IF, N_WHILE, N_FOR, N_BLOCK, N_RETURN,
       N_BREAK, N_CONTINUE, N_EXPR, N_PROGRAM, N_PROP, N_SWITCH, N_CASE, N_DOWHILE, N_FOROF,
       N_TRY, N_THROW, N_FORIN, N_THIS, N_NEW, N_CLASS, N_SUPER, N_SPREAD, N_REGEX };

typedef struct node node;
struct node {
    int type; int64_t num; int op /*single-char or coded*/; const char *str; int slen;
    node *a, *b, *c, *d;
    node **list; int nlist;
    int prefix;   /* for N_UPDATE: prefix vs postfix */
};

static int g_depth;            /* recursion guard (parser + eval + val_to_str + calls) */
/* ~850 B of C stack per nesting level (measured). Cap so the worst case (~120 *
 * 850 B ≈ 100 KB) stays well within the 256 KB kernel stacks BOTH entry paths run
 * on — the ring-3 SYS_js task stack AND the WM/boot stack the browser uses — with
 * margin for the call chain + interrupts, since neither stack has a guard page. */
#define MAXDEPTH 120

/* On arena exhaustion, return a shared dummy node (never NULL) so the parser's
 * field writes (n->a=..., n->op=...) are harmless scribbles rather than NULL
 * derefs; g_oom is set and the parse loops bail. The garbage AST is never run. */
static node g_dummy_node;
static node *mknode(int type){ node *n=aalloc(sizeof(node)); if(!n){ memset(&g_dummy_node,0,sizeof(g_dummy_node)); return &g_dummy_node; } memset(n,0,sizeof(*n)); n->type=type; return n; }

/* ---- parser (recursive descent + precedence climbing) ---- */
static node *parse_expr(lexer *L);
static node *parse_assign(lexer *L);
static node *parse_stmt(lexer *L);
static node *parse_unary(lexer *L);
static node *parse_postfix(lexer *L);
static node *parse_primary(lexer *L);

static void expect_punc(lexer *L, const char *p){ token t=advance(L); if(!(t.type==T_PUNC && tok_is(t,p))) rt_err("syntax: expected punctuation"); }

/* Parse a `( p1, p2 = default, ... )` parameter list into fn->list. Shared by
 * `function`, object-literal method shorthand, and (indirectly) constructors. */
static void parse_fn_params(lexer *L, node *fn) {
    expect_punc(L,"(");
    fn->list = aalloc(sizeof(node*)*32); fn->nlist=0;
    while (!peek_punc(L,")") && peek(L).type!=T_EOF && !g_err && !g_oom) {
        int rest=0; if (peek_punc(L,"...")) { advance(L); rest=1; }   /* ...rest param */
        if (!rest && (peek_punc(L,"[")||peek_punc(L,"{"))) {          /* destructuring param: f([a,b], {c}) */
            node *pat=parse_primary(L);
            if (peek_punc(L,"=")) { advance(L); node *as=mknode(N_ASSIGN); as->op='='; as->a=pat; as->b=parse_assign(L); pat=as; }  /* default for a missing arg */
            if (fn->list && fn->nlist<32) fn->list[fn->nlist++]=pat;
            if (peek_punc(L,",")) advance(L); else break;
            continue;
        }
        token p=advance(L);
        if (p.type==T_IDENT){ node *id=mknode(N_IDENT); id->str=intern(p.s,p.len); id->slen=p.len;
            if (rest) id->op='.';                                      /* marks the rest param */
            else if (peek_punc(L,"=")) { advance(L); id->a=parse_assign(L); }   /* default param value */
            if (fn->list && fn->nlist<32) fn->list[fn->nlist++]=id; }
        if (rest) break;                                              /* rest is the last param */
        if (peek_punc(L,",")) advance(L); else break;
    }
    expect_punc(L,")");
}

static node **parse_list(lexer *L, const char *close, int *count) {
    node **arr = aalloc(sizeof(node*) * 64); int n = 0;
    while (!peek_punc(L, close) && peek(L).type != T_EOF && !g_err && !g_oom) {
        node *el;
        if (peek_punc(L,"...")) { advance(L); el=mknode(N_SPREAD); el->a=parse_assign(L); }  /* ...spread */
        else el=parse_assign(L);
        if (arr && n < 64) arr[n++] = el;
        if (peek_punc(L, ",")) advance(L); else break;
    }
    expect_punc(L, close); *count = n; return arr;
}

static node *mkbin_plus(node *a, node *b){ node *n=mknode(N_BINARY); n->op='+'; n->a=a; n->b=b; return n; }

/* Parse a template literal's raw inner text into a `+`-concatenation of string
 * literals and `${expr}` substitutions. Always begins with a (possibly empty)
 * string literal, so the whole chain coerces to a string. */
static node *parse_template(const char *raw, int len) {
    node *chain = 0;
    char *lit = aalloc(len + 1); int ln = 0;
    #define TPL_FLUSH() do { if(lit) lit[ln]=0; node *sn=mknode(N_STR); sn->str=intern(lit?lit:"",ln); sn->slen=ln; chain = chain ? mkbin_plus(chain,sn) : sn; ln=0; } while(0)
    int i = 0;
    while (i < len) {
        if (raw[i]=='\\' && i+1<len) { char e=raw[i+1]; char c = e=='n'?'\n':e=='t'?'\t':e=='r'?'\r':e=='`'?'`':e=='$'?'$':e=='\\'?'\\':e; if(lit && ln<len) lit[ln++]=c; i+=2; continue; }
        if (raw[i]=='$' && i+1<len && raw[i+1]=='{') {
            TPL_FLUSH();                                  /* literal before the ${ */
            int j=i+2, depth=1; while(j<len && depth>0){ if(raw[j]=='{') depth++; else if(raw[j]=='}'){ depth--; if(depth==0) break; } j++; }
            lexer sub; memset(&sub,0,sizeof(sub)); sub.src=raw+i+2; sub.len=j-(i+2); sub.pos=0;
            node *e = parse_assign(&sub);
            chain = mkbin_plus(chain, e);                 /* chain is non-null after TPL_FLUSH */
            i = (j<len)? j+1 : j;
            continue;
        }
        if (lit && ln<len) lit[ln++]=raw[i];
        i++;
    }
    TPL_FLUSH();                                          /* trailing literal */
    return chain;
}

static node *parse_primary(lexer *L) {
    token t = peek(L);
    if (t.type == T_TEMPLATE) { advance(L); return parse_template(t.s, t.len); }
    if (t.type == T_NUM) { advance(L); node *n=mknode(N_NUM); n->num=t.num; return n; }
    if (t.type == T_STR) { advance(L); node *n=mknode(N_STR); n->str=t.s; n->slen=t.len; return n; }
    if (t.type == T_REGEX) { advance(L); node *n=mknode(N_REGEX); n->str=intern(t.s,t.len); n->slen=t.len; n->num=t.num; return n; }   /* /pattern/flags; num bit0=g bit1=i */
    if (t.type == T_KW) {
        if (tok_is(t,"true")||tok_is(t,"false")) { advance(L); node *n=mknode(N_BOOL); n->num=tok_is(t,"true"); return n; }
        if (tok_is(t,"null")) { advance(L); return mknode(N_NULL); }
        if (tok_is(t,"undefined")) { advance(L); return mknode(N_UNDEF); }
        if (tok_is(t,"function")) {
            advance(L); node *n=mknode(N_FUNC);
            token name = peek(L);
            if (name.type==T_IDENT) { advance(L); n->str=intern(name.s,name.len); n->slen=name.len; }
            parse_fn_params(L, n);
            n->a = parse_stmt(L);   /* body block */
            return n;
        }
        if (tok_is(t,"typeof")) { advance(L); node *n=mknode(N_UNARY); n->op='t'; n->a=parse_primary(L); return n; }
        if (tok_is(t,"this")) { advance(L); return mknode(N_THIS); }
        if (tok_is(t,"super")) { advance(L); return mknode(N_SUPER); }
        if (tok_is(t,"class")) {
            advance(L);
            node *cls=mknode(N_CLASS);
            token nm=peek(L); if(nm.type==T_IDENT){ advance(L); cls->str=intern(nm.s,nm.len); cls->slen=nm.len; }
            if (peek_kw(L,"extends")) { advance(L); cls->a=parse_postfix(L); }   /* superclass expression */
            expect_punc(L,"{");
            cls->list=aalloc(sizeof(node*)*32); cls->nlist=0;
            while (!peek_punc(L,"}") && peek(L).type!=T_EOF && !g_err && !g_oom) {
                if (peek_punc(L,";")) { advance(L); continue; }   /* stray semicolons between members */
                token mn=advance(L);                              /* method name (incl. "constructor") */
                node *fn=mknode(N_FUNC); fn->str=intern(mn.s,mn.len); fn->slen=mn.len;
                parse_fn_params(L, fn);
                fn->a = parse_stmt(L);                            /* method body */
                if (cls->list && cls->nlist<32) cls->list[cls->nlist++]=fn;
            }
            expect_punc(L,"}");
            return cls;
        }
        if (tok_is(t,"new")) {
            advance(L);
            node *callee = parse_primary(L);
            /* member chain (a.b.C / a[k]) binds to `new`, but a `(` opens the
             * constructor's own arg list — so we stop the chain before calls. */
            for (;;) {
                if (peek_punc(L,".")) { advance(L); token p=advance(L); node *m=mknode(N_MEMBER); m->a=callee; m->str=intern(p.s,p.len); m->slen=p.len; callee=m; }
                else if (peek_punc(L,"[")) { advance(L); node *idx=parse_expr(L); expect_punc(L,"]"); node *m=mknode(N_INDEX); m->a=callee; m->b=idx; callee=m; }
                else break;
            }
            node *nw = mknode(N_NEW); nw->a = callee;
            if (peek_punc(L,"(")) { advance(L); nw->list = parse_list(L,")",&nw->nlist); }
            return nw;
        }
    }
    if (t.type == T_IDENT) { advance(L); node *n=mknode(N_IDENT); n->str=intern(t.s,t.len); n->slen=t.len; return n; }
    if (t.type == T_PUNC) {
        if (tok_is(t,"(")) { advance(L); node *e=parse_expr(L); expect_punc(L,")"); return e; }
        if (tok_is(t,"[")) { advance(L); node *n=mknode(N_ARRAY); n->list=parse_list(L,"]",&n->nlist); return n; }
        if (tok_is(t,"{")) {
            advance(L); node *n=mknode(N_OBJECT); n->list=aalloc(sizeof(node*)*64); n->nlist=0;
            while (!peek_punc(L,"}") && peek(L).type!=T_EOF && !g_err && !g_oom) {
                if (peek_punc(L,"...")) { advance(L); node *sp=mknode(N_SPREAD); sp->a=parse_assign(L); if(n->list && n->nlist<64) n->list[n->nlist++]=sp; if(peek_punc(L,",")) advance(L); continue; }  /* {...obj} */
                if (peek_punc(L,"[")) {   /* computed key: {[expr]: value} (pr->b holds the key expression) */
                    advance(L); node *pr=mknode(N_PROP); pr->b=parse_assign(L); expect_punc(L,"]"); expect_punc(L,":"); pr->a=parse_assign(L);
                    if (n->list && n->nlist<64) n->list[n->nlist++]=pr;
                    if (peek_punc(L,",")) advance(L); else break;
                    continue;
                }
                token k=advance(L); node *pr=mknode(N_PROP);
                pr->str=intern(k.s,k.len); pr->slen=k.len;
                if (peek_punc(L,":")) { advance(L); pr->a=parse_assign(L); }
                else if (peek_punc(L,"(")) { node *fn=mknode(N_FUNC); parse_fn_params(L,fn); fn->a=parse_stmt(L); pr->a=fn; }   /* method shorthand: name(args){…} */
                else if (peek_punc(L,"=")) { advance(L); node *id=mknode(N_IDENT); id->str=pr->str; id->slen=pr->slen; node *as=mknode(N_ASSIGN); as->op='='; as->a=id; as->b=parse_assign(L); pr->a=as; }   /* {x = default} (destructuring) */
                else { node *id=mknode(N_IDENT); id->str=pr->str; id->slen=pr->slen; pr->a=id; }   /* {x} shorthand == {x:x} */
                if (n->list && n->nlist<64) n->list[n->nlist++]=pr;
                if (peek_punc(L,",")) advance(L); else break;
            }
            expect_punc(L,"}"); return n;
        }
    }
    rt_err("syntax: unexpected token"); advance(L); return mknode(N_UNDEF);
}

static node *parse_postfix(lexer *L) {
    node *e = parse_primary(L);
    int opt = 0;   /* once a `?.` appears, the rest of this chain short-circuits on a nullish
                    * receiver too (so `a?.b.c()` yields undefined when a is null, per spec).
                    * Marking a link optional only ADDS a nullish guard; non-null behaviour is
                    * unchanged, so propagating it to later links is safe. */
    for (;;) {
        if (peek_punc(L,".")) { advance(L); token p=advance(L); node *m=mknode(N_MEMBER); m->a=e; m->str=intern(p.s,p.len); m->slen=p.len; m->prefix=opt; e=m; }
        else if (peek_punc(L,"?.")) { opt=1; advance(L);   /* optional chaining: ?.x  ?.[i]  ?.() */
            if (peek_punc(L,"(")) { advance(L); node *call=mknode(N_CALL); call->a=e; call->prefix=1; call->list=parse_list(L,")",&call->nlist); e=call; }
            else if (peek_punc(L,"[")) { advance(L); node *idx=parse_expr(L); expect_punc(L,"]"); node *m=mknode(N_INDEX); m->a=e; m->b=idx; m->prefix=1; e=m; }
            else { token p=advance(L); node *m=mknode(N_MEMBER); m->a=e; m->str=intern(p.s,p.len); m->slen=p.len; m->prefix=1; e=m; }
        }
        else if (peek_punc(L,"[")) { advance(L); node *idx=parse_expr(L); expect_punc(L,"]"); node *m=mknode(N_INDEX); m->a=e; m->b=idx; m->prefix=opt; e=m; }
        else if (peek_punc(L,"(")) { advance(L); node *call=mknode(N_CALL); call->a=e; call->prefix=opt; call->list=parse_list(L,")",&call->nlist); e=call; }
        else if (peek_punc(L,"++")||peek_punc(L,"--")) { token o=advance(L); node *u=mknode(N_UPDATE); u->op=o.s[0]; u->a=e; u->prefix=0; e=u; }
        else break;
    }
    return e;
}

static node *parse_unary_inner(lexer *L) {
    if (peek_punc(L,"!")||peek_punc(L,"-")||peek_punc(L,"+")) { token o=advance(L); node *u=mknode(N_UNARY); u->op=o.s[0]; u->a=parse_unary(L); return u; }
    if (peek_punc(L,"++")||peek_punc(L,"--")) { token o=advance(L); node *u=mknode(N_UPDATE); u->op=o.s[0]; u->prefix=1; u->a=parse_unary(L); return u; }
    if (peek_kw(L,"typeof")) { advance(L); node *u=mknode(N_UNARY); u->op='t'; u->a=parse_unary(L); return u; }
    return parse_postfix(L);
}
/* depth-guarded wrapper: a `!!!!...` / `typeof typeof...` / `- - -...` chain
 * otherwise recurses C-stack-deep with no bound (parse_assign only guards via the
 * paren/binary paths). MAXDEPTH protects the guard-page-less kernel stack. */
static node *parse_unary(lexer *L) {
    if (++g_depth > MAXDEPTH) { rt_err("expression nested too deep"); g_depth--; return mknode(N_UNDEF); }
    node *r = parse_unary_inner(L); g_depth--; return r;
}

/* binary precedence */
static int bin_prec(token t, int *code) {
    if (t.type!=T_PUNC) return 0;
    if (tok_is(t,"*")||tok_is(t,"/")||tok_is(t,"%")) { *code=t.s[0]; return 11; }
    if (tok_is(t,"+")||tok_is(t,"-")) { *code=t.s[0]; return 10; }
    if (tok_is(t,"<<")||tok_is(t,">>")) { *code=(t.s[0]=='<')?'L':'R'; return 9; }
    if (tok_is(t,"<")||tok_is(t,">")) { *code=t.s[0]; return 8; }
    if (tok_is(t,"<=")) { *code='l'; return 8; } if (tok_is(t,">=")) { *code='g'; return 8; }
    if (tok_is(t,"===")||tok_is(t,"==")) { *code='='; return 7; }
    if (tok_is(t,"!==")||tok_is(t,"!=")) { *code='!'; return 7; }
    if (tok_is(t,"&")) { *code='&'; return 6; }
    if (tok_is(t,"|")) { *code='|'; return 4; }
    if (tok_is(t,"&&")) { *code='A'; return 3; }
    if (tok_is(t,"||")) { *code='O'; return 2; }
    if (tok_is(t,"??")) { *code='N'; return 2; }   /* nullish coalescing */
    return 0;
}

static node *parse_binary(lexer *L, int minp) {
    node *left = parse_unary(L);
    for (;;) {
        int code; int p = bin_prec(peek(L), &code);
        if (p == 0 || p < minp) break;
        advance(L);
        node *right = parse_binary(L, p+1);
        int logical = (code=='A'||code=='O'||code=='N');
        node *n = mknode(logical?N_LOGICAL:N_BINARY); n->op=code; n->a=left; n->b=right;
        left = n;
    }
    return left;
}

static node *parse_cond(lexer *L) {
    node *c = parse_binary(L, 1);
    if (peek_punc(L,"?")) { advance(L); node *n=mknode(N_COND); n->a=c; n->b=parse_assign(L); expect_punc(L,":"); n->c=parse_assign(L); return n; }
    return c;
}

/* Build an arrow function N_FUNC from already-parsed params; parses the body
 * (a `{ }` block, or an expression that becomes an implicit `return`). */
static node *make_arrow(lexer *L, node **params, int np) {
    node *fn = mknode(N_FUNC);
    fn->prefix = 1;   /* mark as arrow: inherits `this` lexically (no own binding) */
    fn->list = aalloc((long)sizeof(node*) * (np>0?np:1)); fn->nlist = np;
    for (int i=0;i<np;i++) fn->list[i]=params[i];
    if (peek_punc(L,"{")) { fn->a = parse_stmt(L); }
    else { node *ret=mknode(N_RETURN); ret->a=parse_assign(L);
           node *blk=mknode(N_BLOCK); blk->list=aalloc(sizeof(node*)); if(blk->list){blk->list[0]=ret; blk->nlist=1;} fn->a=blk; }
    return fn;
}

static node *parse_assign(lexer *L) {
    if (++g_depth > MAXDEPTH) { rt_err("max recursion"); g_depth--; return mknode(N_UNDEF); }
    /* arrow functions: `x => body` and `(a, b, ...) => body`. Detected with bounded
     * lookahead (save/restore the lexer) so a plain `(expr)` isn't misparsed. */
    token t0 = peek(L);
    if (t0.type==T_IDENT) {
        lexsave sv = lex_save(L); token id = advance(L);
        if (peek_punc(L,"=>")) { advance(L); node *p=mknode(N_IDENT); p->str=intern(id.s,id.len); p->slen=id.len;
                                 node *ps[1]={p}; node *fn=make_arrow(L,ps,1); g_depth--; return fn; }
        lex_restore(L, sv);
    } else if (t0.type==T_PUNC && tok_is(t0,"(")) {
        lexsave sv = lex_save(L); advance(L);
        node *ps[16]; int np=0, ok=1;
        if (!peek_punc(L,")")) for(;;){ int rest=0; if(peek_punc(L,"...")){ advance(L); rest=1; }
            if (!rest && (peek_punc(L,"[")||peek_punc(L,"{"))) {   /* ([a,b])=>… / ({x})=>… destructuring param */
                node *pat=parse_primary(L);
                if (peek_punc(L,"=")) { advance(L); node *as=mknode(N_ASSIGN); as->op='='; as->a=pat; as->b=parse_assign(L); pat=as; }
                if(np<16) ps[np++]=pat;
                if(peek_punc(L,",")) { advance(L); continue; } else break;
            }
            token p=peek(L); if(p.type!=T_IDENT){ ok=0; break; } advance(L);
            if(np<16){ node *id=mknode(N_IDENT); id->str=intern(p.s,p.len); id->slen=p.len; if(rest) id->op='.'; ps[np++]=id; }
            if(rest) break;   /* ...rest is the last param */
            if(peek_punc(L,",")) advance(L); else break; }
        if (ok && peek_punc(L,")")) { advance(L); if (peek_punc(L,"=>")) { advance(L); node *fn=make_arrow(L,ps,np); g_depth--; return fn; } }
        lex_restore(L, sv);
    }
    node *left = parse_cond(L);
    token t = peek(L);
    if (t.type==T_PUNC && (tok_is(t,"=")||tok_is(t,"+=")||tok_is(t,"-=")||tok_is(t,"*=")||tok_is(t,"/=")||tok_is(t,"%="))) {
        advance(L); node *n=mknode(N_ASSIGN); n->op = (t.len==1)?'=':t.s[0]; n->a=left; n->b=parse_assign(L); g_depth--; return n;
    }
    g_depth--; return left;
}

static node *parse_expr(lexer *L) {
    node *e = parse_assign(L);
    while (peek_punc(L,",")) { advance(L); e = parse_assign(L); }   /* comma: keep last */
    return e;
}

static node *parse_block(lexer *L) {
    expect_punc(L,"{"); node *n=mknode(N_BLOCK); n->list=aalloc(sizeof(node*)*256); n->nlist=0;
    for (;;) { skip_semi(L); if (peek_punc(L,"}")||peek(L).type==T_EOF||g_err||g_oom) break; node *s=parse_stmt(L); if(n->list&&n->nlist<256) n->list[n->nlist++]=s; }
    expect_punc(L,"}"); return n;
}

static node *parse_var(lexer *L) {
    advance(L);  /* var/let/const */
    node *n=mknode(N_VAR); n->list=aalloc(sizeof(node*)*32); n->nlist=0;
    for (;;) {
        node *decl=mknode(N_PROP);
        if (peek_punc(L,"[")||peek_punc(L,"{")) { decl->b=parse_primary(L); }   /* [..]/{..} destructuring pattern */
        else { token id=advance(L); decl->str=intern(id.s,id.len); decl->slen=id.len; }
        if (peek_punc(L,"=")) { advance(L); decl->a=parse_assign(L); }
        if (n->list && n->nlist<32) n->list[n->nlist++]=decl;
        if (g_err || g_oom) break;
        if (peek_punc(L,",")) advance(L); else break;
    }
    return n;
}

static node *parse_stmt(lexer *L) {
    if (++g_depth > MAXDEPTH) { rt_err("max recursion"); g_depth--; return mknode(N_UNDEF); }
    node *r;
    if (peek_punc(L,"{")) { r=parse_block(L); g_depth--; return r; }
    if (peek_kw(L,"var")||peek_kw(L,"let")||peek_kw(L,"const")) { r=parse_var(L); g_depth--; return r; }
    if (peek_kw(L,"function")) { r=parse_primary(L); g_depth--; return r; }   /* function decl */
    if (peek_kw(L,"return")) { advance(L); node *n=mknode(N_RETURN); if(!peek_punc(L,"}")&&!peek_punc(L,";")&&peek(L).type!=T_EOF) n->a=parse_expr(L); g_depth--; return n; }
    if (peek_kw(L,"break")) { advance(L); g_depth--; return mknode(N_BREAK); }
    if (peek_kw(L,"continue")) { advance(L); g_depth--; return mknode(N_CONTINUE); }
    if (peek_kw(L,"if")) {
        advance(L); expect_punc(L,"("); node *n=mknode(N_IF); n->a=parse_expr(L); expect_punc(L,")");
        n->b=parse_stmt(L); skip_semi(L);   /* a ; may terminate the then-branch before else */
        if (peek_kw(L,"else")) { advance(L); n->c=parse_stmt(L); } g_depth--; return n;
    }
    if (peek_kw(L,"while")) { advance(L); expect_punc(L,"("); node *n=mknode(N_WHILE); n->a=parse_expr(L); expect_punc(L,")"); n->b=parse_stmt(L); g_depth--; return n; }
    if (peek_kw(L,"try")) {
        advance(L); node *n=mknode(N_TRY); n->a=parse_block(L);
        if (peek_kw(L,"catch")) { advance(L);
            if (peek_punc(L,"(")) { advance(L); token p=advance(L); if(p.type==T_IDENT){ n->str=intern(p.s,p.len); n->slen=p.len; } expect_punc(L,")"); }
            n->b=parse_block(L); }
        if (peek_kw(L,"finally")) { advance(L); n->c=parse_block(L); }
        g_depth--; return n;
    }
    if (peek_kw(L,"throw")) { advance(L); node *n=mknode(N_THROW); n->a=parse_expr(L); g_depth--; return n; }
    if (peek_kw(L,"do")) { advance(L); node *n=mknode(N_DOWHILE); n->b=parse_stmt(L); skip_semi(L);
        if (peek_kw(L,"while")) advance(L); expect_punc(L,"("); n->a=parse_expr(L); expect_punc(L,")"); g_depth--; return n; }
    if (peek_kw(L,"switch")) {
        advance(L); expect_punc(L,"("); node *n=mknode(N_SWITCH); n->a=parse_expr(L); expect_punc(L,")"); expect_punc(L,"{");
        n->list=aalloc(sizeof(node*)*64); n->nlist=0;
        while (!peek_punc(L,"}") && peek(L).type!=T_EOF && !g_err && !g_oom) {
            node *cl=mknode(N_CASE);
            if (peek_kw(L,"case")) { advance(L); cl->a=parse_expr(L); expect_punc(L,":"); }
            else if (peek_kw(L,"default")) { advance(L); cl->a=0; expect_punc(L,":"); }
            else { advance(L); continue; }                /* tolerate stray tokens */
            cl->list=aalloc(sizeof(node*)*64); cl->nlist=0;
            for (;;) { skip_semi(L); if (peek_kw(L,"case")||peek_kw(L,"default")||peek_punc(L,"}")||peek(L).type==T_EOF||g_err||g_oom) break;
                node *s=parse_stmt(L); if (cl->list && cl->nlist<64) cl->list[cl->nlist++]=s; }
            if (n->list && n->nlist<64) n->list[n->nlist++]=cl;
        }
        expect_punc(L,"}"); g_depth--; return n;
    }
    if (peek_kw(L,"for")) {
        advance(L); expect_punc(L,"(");
        /* for (x of iterable) — detect the contextual `of` with bounded lookahead */
        lexsave sv = lex_save(L);
        if (peek_kw(L,"var")||peek_kw(L,"let")||peek_kw(L,"const")) advance(L);
        if (peek_punc(L,"[")||peek_punc(L,"{")) {   /* for (var [a,b] of ...) — destructuring loop var */
            node *pat=parse_primary(L); token kw=peek(L);
            if (kw.type==T_IDENT && kw.len==2 && kw.s[0]=='o' && kw.s[1]=='f') {
                advance(L); node *fo=mknode(N_FOROF); fo->c=pat;
                fo->a=parse_expr(L); expect_punc(L,")"); fo->b=parse_stmt(L); g_depth--; return fo; }
        } else {
            token v = peek(L);
            if (v.type==T_IDENT) { advance(L); token kw = peek(L);
                int isof = (kw.type==T_IDENT && kw.len==2 && kw.s[0]=='o' && kw.s[1]=='f');
                int isin = (kw.type==T_IDENT && kw.len==2 && kw.s[0]=='i' && kw.s[1]=='n');
                if (isof || isin) {
                    advance(L); node *fo=mknode(isof?N_FOROF:N_FORIN); fo->str=intern(v.s,v.len); fo->slen=v.len;
                    fo->a=parse_expr(L); expect_punc(L,")"); fo->b=parse_stmt(L); g_depth--; return fo; }
            }
        }
        lex_restore(L, sv);
        node *n=mknode(N_FOR);
        if (!peek_punc(L,";")) { if (peek_kw(L,"var")||peek_kw(L,"let")||peek_kw(L,"const")) n->a=parse_var(L); else n->a=parse_expr(L); }
        expect_punc(L,";");
        if (!peek_punc(L,";")) n->b=parse_expr(L);
        expect_punc(L,";");
        if (!peek_punc(L,")")) n->c=parse_expr(L);
        expect_punc(L,")");
        n->d=parse_stmt(L); g_depth--; return n;
    }
    node *e=mknode(N_EXPR); e->a=parse_expr(L); g_depth--; return e;
}

static node *parse_program(lexer *L) {
    node *n=mknode(N_PROGRAM); n->list=aalloc(sizeof(node*)*1024); n->nlist=0;
    for (;;) { skip_semi(L); if (peek(L).type==T_EOF||g_err||g_oom) break; node *s=parse_stmt(L); if(n->list&&n->nlist<1024) n->list[n->nlist++]=s; }
    return n;
}

/* =========================== values =========================== */
enum { V_UNDEF, V_NULL, V_BOOL, V_NUM, V_STR, V_OBJ, V_ARR, V_FUN, V_NATIVE,
       V_MAP, V_SET, V_REGEX /* obj->kind markers only; the val.t stays V_OBJ */ };
typedef struct val val;
typedef struct obj obj;
typedef struct env env;

struct val { int t; int64_t num; const char *str; obj *o; };

struct obj {
    int kind;
    /* object: parallel key/val arrays */
    const char **keys; val *vals; int n, cap;
    /* function */
    node *fn; env *scope;
    val (*native)(val *args, int nargs);
    obj *home_proto;   /* class constructors: an object holding the methods to copy onto each new instance */
    obj *super_class;  /* a class method/ctor's parent constructor, for super() / super.m() */
    void *rx;          /* compiled regex (struct regex*) when kind==V_REGEX */
};

struct env { const char **keys; val *vals; int n, cap; env *parent; };

static val UND(void){ val v; v.t=V_UNDEF; v.num=0; v.str=0; v.o=0; return v; }
static val NUM(int64_t x){ val v=UND(); v.t=V_NUM; v.num=x; return v; }
static val BOOLV(int b){ val v=UND(); v.t=V_BOOL; v.num=b?1:0; return v; }
static val STRV(const char *s){ val v=UND(); v.t=V_STR; v.str=s?s:""; return v; }
static val g_throwval;        /* value of the in-flight `throw` (when g_threw) */

static obj *new_obj(int kind){ obj *o=aalloc(sizeof(obj)); if(!o) return 0; memset(o,0,sizeof(*o)); o->kind=kind; o->cap=4; o->keys=aalloc(sizeof(char*)*o->cap); o->vals=aalloc(sizeof(val)*o->cap); if(!o->keys||!o->vals){ g_oom=1; return 0; } return o; }

static void obj_set(obj *o, const char *key, val v) {
    if (!o || !o->keys || !o->vals) { g_oom=1; return; }   /* a NULL/half-built obj (OOM) — don't deref */
    for (int i=0;i<o->n;i++) if (strcmp(o->keys[i],key)==0) { o->vals[i]=v; return; }
    if (o->n>=o->cap) { int nc=o->cap*2; const char **nk=aalloc(sizeof(char*)*nc); val *nv=aalloc(sizeof(val)*nc); if(!nk||!nv){g_oom=1;return;} memcpy(nk,o->keys,sizeof(char*)*o->n); memcpy(nv,o->vals,sizeof(val)*o->n); o->keys=nk; o->vals=nv; o->cap=nc; }
    o->keys[o->n]=key; o->vals[o->n]=v; o->n++;
}
static int obj_get(obj *o, const char *key, val *out) {
    for (int i=0;i<o->n;i++) if (strcmp(o->keys[i],key)==0) { *out=o->vals[i]; return 1; }
    return 0;
}
static void arr_push_val(obj *o, val v) {
    if (o->n >= o->cap) { int nc=o->cap*2+2; val *nv=aalloc((long)sizeof(val)*nc); if(!nv){g_oom=1;return;} memcpy(nv,o->vals,sizeof(val)*o->n); o->vals=nv; o->cap=nc; }
    o->vals[o->n++]=v;
}
static val obj_val(obj *o);          /* fwd (defined near install_globals) */
static val obj_val_native(obj *o);

/* ---- number/string helpers ---- */
static char *i64_to_str(int64_t v) {
    char tmp[24]; int i=0; int neg = v<0; uint64_t u = neg?(uint64_t)(-(v+1))+1:(uint64_t)v;
    if (u==0) tmp[i++]='0';
    while (u){ tmp[i++]='0'+(int)(u%10); u/=10; }
    if (neg) tmp[i++]='-';
    char *s=aalloc(i+1); if(!s) return ""; for(int j=0;j<i;j++) s[j]=tmp[i-1-j]; s[i]=0; return s;
}
static const char *val_to_str(val v); /* fwd */
static int truthy(val v) {
    switch (v.t) {
        case V_UNDEF: case V_NULL: return 0;
        case V_BOOL: case V_NUM: return v.num!=0;
        case V_STR: return v.str && v.str[0];
        default: return 1;
    }
}
static int64_t to_num(val v) {
    switch (v.t) {
        case V_NUM: case V_BOOL: return v.num;
        case V_STR: { int64_t x=0; const char*s=v.str; int neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){x=x*10+(*s-'0');s++;} return neg?-x:x; }
        default: return 0;
    }
}
/* `===`-style equality, used for Map keys and Set members: primitives by value,
 * objects/functions by identity (same obj pointer). */
static int val_equal(val a, val b) {
    if (a.t!=b.t) return 0;          /* strict (===): 1 !== true, etc. */
    switch (a.t) {
        case V_UNDEF: case V_NULL: return 1;
        case V_NUM: case V_BOOL: return a.num==b.num;
        case V_STR: return a.str && b.str && strcmp(a.str,b.str)==0;
        default: return a.o==b.o;   /* V_OBJ/V_ARR/V_FUN/V_NATIVE: identity */
    }
}

/* =========================== regular expressions ===========================
 * A from-scratch regex: pattern -> tree -> a small instruction program, run by
 * a recursive backtracking matcher with a STEP BUDGET + DEPTH CAP so a
 * pathological pattern on untrusted input fails gracefully instead of hanging or
 * overflowing the kernel stack (verified on `(a+)+$`). Supports literals, . ,
 * [classes] (ranges, negation, \d\w\s\D\W\S), * + ? (greedy), | , (capture
 * groups), ^ $, escapes; flags i (ignore case) and g (global). */
enum { I_CHAR, I_ANY, I_CLASS, I_BOL, I_EOL, I_SAVE, I_SPLIT, I_JMP, I_MATCH };
typedef struct { int op; int c; int x, y; unsigned char *cls; } reinst;
#define RE_MAXPROG 512
#define RE_MAXGROUP 9
typedef struct { reinst *prog; int n; int ngroup; int icase; int global; int lastIndex; const char *source; int ok; } regex;

enum { RN_CHAR, RN_ANY, RN_CLASS, RN_BOL, RN_EOL, RN_CAT, RN_ALT, RN_STAR, RN_PLUS, RN_OPT, RN_GROUP, RN_EMPTY };
typedef struct rnode rnode;
struct rnode { int type; int c; unsigned char *cls; rnode *a, *b; int group; };
typedef struct { const char *p; int len, pos; int ngroup; int err; int depth; } rparse;

static rnode *rx_node(int t){ rnode *n=aalloc(sizeof(rnode)); if(!n) return 0; memset(n,0,sizeof(*n)); n->type=t; return n; }
static rnode *rx_alt(rparse *P);
static void cls_set(unsigned char *cls,int c){ cls[(c&0xff)>>3] |= 1<<(c&7); }
static void cls_class(unsigned char *cls,int kind){
    if(kind=='d'){ for(int c='0';c<='9';c++) cls_set(cls,c); }
    else if(kind=='w'){ for(int c='0';c<='9';c++) cls_set(cls,c); for(int c='a';c<='z';c++) cls_set(cls,c); for(int c='A';c<='Z';c++) cls_set(cls,c); cls_set(cls,'_'); }
    else if(kind=='s'){ cls_set(cls,' '); cls_set(cls,'\t'); cls_set(cls,'\n'); cls_set(cls,'\r'); cls_set(cls,'\f'); cls_set(cls,'\v'); }
}
static rnode *rx_class(rparse *P){
    rnode *n=rx_node(RN_CLASS); if(!n){P->err=1;return 0;} n->cls=aalloc(32); if(!n->cls){P->err=1;return 0;} memset(n->cls,0,32);
    int neg=0; if(P->pos<P->len && P->p[P->pos]=='^'){ neg=1; P->pos++; }
    while(P->pos<P->len && P->p[P->pos]!=']'){
        int c=(unsigned char)P->p[P->pos++];
        if(c=='\\' && P->pos<P->len){ int e=(unsigned char)P->p[P->pos++];
            if(e=='d'||e=='w'||e=='s'){ cls_class(n->cls,e); continue; }
            if(e=='D'||e=='W'||e=='S'){ unsigned char tmp[32]; memset(tmp,0,32); cls_class(tmp,e+32); for(int i=0;i<32;i++) n->cls[i]|=~tmp[i]; continue; }
            if(e=='n')c='\n'; else if(e=='t')c='\t'; else if(e=='r')c='\r'; else c=e;
        }
        if(P->pos+1<P->len && P->p[P->pos]=='-' && P->p[P->pos+1]!=']'){ P->pos++; int hi=(unsigned char)P->p[P->pos++]; if(hi=='\\'&&P->pos<P->len) hi=(unsigned char)P->p[P->pos++]; for(int x=c;x<=hi;x++) cls_set(n->cls,x); }
        else cls_set(n->cls,c);
    }
    if(P->pos<P->len && P->p[P->pos]==']') P->pos++; else P->err=1;
    n->c=neg; return n;
}
static rnode *rx_atom(rparse *P){
    if(P->pos>=P->len) return rx_node(RN_EMPTY);
    int c=(unsigned char)P->p[P->pos];
    if(c=='('){ P->pos++; if(P->pos+1<P->len && P->p[P->pos]=='?' && P->p[P->pos+1]==':') P->pos+=2; int gi=(P->ngroup<RE_MAXGROUP)?++P->ngroup:0; rnode *body=rx_alt(P); if(P->pos<P->len&&P->p[P->pos]==')')P->pos++; else P->err=1; rnode *g=rx_node(RN_GROUP); if(!g){P->err=1;return 0;} g->a=body; g->group=gi; return g; }
    if(c=='['){ P->pos++; return rx_class(P); }
    if(c=='.'){ P->pos++; return rx_node(RN_ANY); }
    if(c=='^'){ P->pos++; return rx_node(RN_BOL); }
    if(c=='$'){ P->pos++; return rx_node(RN_EOL); }
    if(c=='\\' && P->pos+1<P->len){ P->pos++; int e=(unsigned char)P->p[P->pos++];
        if(e=='d'||e=='w'||e=='s'){ rnode *n=rx_node(RN_CLASS); if(!n){P->err=1;return 0;} n->cls=aalloc(32); if(!n->cls){P->err=1;return 0;} memset(n->cls,0,32); cls_class(n->cls,e); n->c=0; return n; }
        if(e=='D'||e=='W'||e=='S'){ rnode *n=rx_node(RN_CLASS); if(!n){P->err=1;return 0;} n->cls=aalloc(32); if(!n->cls){P->err=1;return 0;} memset(n->cls,0,32); cls_class(n->cls,e+32); n->c=1; return n; }
        rnode *n=rx_node(RN_CHAR); if(!n){P->err=1;return 0;} if(e=='n')n->c='\n'; else if(e=='t')n->c='\t'; else if(e=='r')n->c='\r'; else n->c=e; return n; }
    P->pos++; rnode *n=rx_node(RN_CHAR); if(!n){P->err=1;return 0;} n->c=c; return n;
}
static rnode *rx_rep(rparse *P){
    rnode *a=rx_atom(P); if(!a) return 0;
    while(P->pos<P->len){ int c=P->p[P->pos];
        if(c=='*'||c=='+'||c=='?'){ P->pos++; rnode *q=rx_node(c=='*'?RN_STAR:c=='+'?RN_PLUS:RN_OPT); if(!q){P->err=1;return 0;} q->a=a; a=q; }
        else break; }
    return a;
}
static rnode *rx_cat(rparse *P){
    rnode *a=0;
    while(P->pos<P->len && P->p[P->pos]!='|' && P->p[P->pos]!=')'){ rnode *r=rx_rep(P); if(P->err) return a; if(!a) a=r; else { rnode *c=rx_node(RN_CAT); if(!c){P->err=1;return a;} c->a=a; c->b=r; a=c; } }
    return a?a:rx_node(RN_EMPTY);
}
static rnode *rx_alt(rparse *P){
    /* group nesting recurses here (rx_atom -> rx_alt for `(`); the pattern is an
     * ordinary string so the interpreter's MAXDEPTH doesn't bound it. Cap it well
     * below the ~1700-group C-stack cliff on the kernel's 256 KB JS stack. */
    if(++P->depth > 400){ P->err=1; P->depth--; return rx_node(RN_EMPTY); }
    rnode *a=rx_cat(P);
    while(P->pos<P->len && P->p[P->pos]=='|'){ P->pos++; rnode *b=rx_cat(P); rnode *alt=rx_node(RN_ALT); if(!alt){P->err=1;break;} alt->a=a; alt->b=b; a=alt; }
    P->depth--;
    return a;
}
typedef struct { reinst *prog; int pc; int err; } remit;
static int rx_emit(remit *E,int op,int c,int x,int y,unsigned char*cls){ if(E->pc>=RE_MAXPROG){E->err=1;return 0;} int at=E->pc++; E->prog[at].op=op; E->prog[at].c=c; E->prog[at].x=x; E->prog[at].y=y; E->prog[at].cls=cls; return at; }
static void rx_compile(remit *E, rnode *n){
    if(!n||E->err) return;
    switch(n->type){
        case RN_CHAR: rx_emit(E,I_CHAR,n->c,0,0,0); break;
        case RN_ANY: rx_emit(E,I_ANY,0,0,0,0); break;
        case RN_CLASS: rx_emit(E,I_CLASS,n->c,0,0,n->cls); break;
        case RN_BOL: rx_emit(E,I_BOL,0,0,0,0); break;
        case RN_EOL: rx_emit(E,I_EOL,0,0,0,0); break;
        case RN_EMPTY: break;
        case RN_CAT: rx_compile(E,n->a); rx_compile(E,n->b); break;
        case RN_GROUP: if(n->group){ rx_emit(E,I_SAVE,2*n->group,0,0,0); rx_compile(E,n->a); rx_emit(E,I_SAVE,2*n->group+1,0,0,0); } else rx_compile(E,n->a); break;
        case RN_STAR: { int l1=rx_emit(E,I_SPLIT,0,0,0,0); rx_compile(E,n->a); rx_emit(E,I_JMP,0,l1,0,0); E->prog[l1].x=l1+1; E->prog[l1].y=E->pc; break; }
        case RN_PLUS: { int l1=E->pc; rx_compile(E,n->a); int sp=rx_emit(E,I_SPLIT,0,l1,0,0); E->prog[sp].y=E->pc; break; }
        case RN_OPT: { int l1=rx_emit(E,I_SPLIT,0,0,0,0); rx_compile(E,n->a); E->prog[l1].x=l1+1; E->prog[l1].y=E->pc; break; }
        case RN_ALT: { int l1=rx_emit(E,I_SPLIT,0,0,0,0); rx_compile(E,n->a); int j=rx_emit(E,I_JMP,0,0,0,0); E->prog[l1].x=l1+1; E->prog[l1].y=E->pc; rx_compile(E,n->b); E->prog[j].x=E->pc; break; }
    }
}
static regex *re_compile(const char *pat, const char *flags){
    regex *re=aalloc(sizeof(regex)); if(!re) return 0; memset(re,0,sizeof(*re));
    if(flags) for(const char*f=flags;*f;f++){ if(*f=='i')re->icase=1; else if(*f=='g')re->global=1; }
    re->source=pat?pat:"";
    rparse P; memset(&P,0,sizeof(P)); P.p=pat?pat:""; P.len=(int)strlen(P.p);
    rnode *tree=rx_alt(&P);
    if(P.err || P.pos!=P.len){ re->ok=0; return re; }
    re->ngroup=P.ngroup;
    remit E; E.prog=aalloc((long)sizeof(reinst)*RE_MAXPROG); if(!E.prog){re->ok=0;return re;} E.pc=0; E.err=0;
    rx_emit(&E,I_SAVE,0,0,0,0); rx_compile(&E,tree); rx_emit(&E,I_SAVE,1,0,0,0); rx_emit(&E,I_MATCH,0,0,0,0);
    if(E.err){ re->ok=0; return re; }
    re->prog=E.prog; re->n=E.pc; re->ok=1; return re;
}
static int rx_eqc(int a,int b,int icase){ if(a==b) return 1; if(icase){ int la=(a>='A'&&a<='Z')?a+32:a, lb=(b>='A'&&b<='Z')?b+32:b; return la==lb; } return 0; }
static int re_run(regex *re,int pc,const char*s,int slen,int sp,int*caps,long*budget,int depth){
    /* re_run recurses per I_SPLIT/I_SAVE, so a greedy quantifier matching N chars
     * recurses ~N deep. The kernel JS task stack is 256 KB with NO guard page and
     * already holds the interpreter's eval frames, so cap conservatively (~900) —
     * the host's 8 MB stack tolerated 3000, the kernel's does not. A single run
     * longer than this fails to match rather than corrupting kernel memory. */
    if(--*budget<0 || depth>900) return -2;
    for(;;){
        reinst *in=&re->prog[pc];
        switch(in->op){
            case I_CHAR: if(sp<slen && rx_eqc((unsigned char)s[sp],in->c,re->icase)){ sp++; pc++; continue; } return 0;
            case I_ANY: if(sp<slen && s[sp]!='\n'){ sp++; pc++; continue; } return 0;
            case I_CLASS: { if(sp>=slen) return 0; unsigned char ch=(unsigned char)s[sp]; int hit=(in->cls[ch>>3]>>(ch&7))&1;
                if(re->icase && !hit){ int o=(ch>='a'&&ch<='z')?ch-32:(ch>='A'&&ch<='Z')?ch+32:ch; hit=(in->cls[(o&0xff)>>3]>>(o&7))&1; }
                if(in->c) hit=!hit; if(hit){ sp++; pc++; continue; } return 0; }
            case I_BOL: if(sp==0 || s[sp-1]=='\n'){ pc++; continue; } return 0;
            case I_EOL: if(sp==slen || s[sp]=='\n'){ pc++; continue; } return 0;
            case I_JMP: pc=in->x; continue;
            case I_SPLIT: { int r=re_run(re,in->x,s,slen,sp,caps,budget,depth+1); if(r!=0) return r; pc=in->y; continue; }
            case I_SAVE: { int idx=in->c; int old=(idx<2*(RE_MAXGROUP+1))?caps[idx]:-1; if(idx<2*(RE_MAXGROUP+1)) caps[idx]=sp;
                int r=re_run(re,pc+1,s,slen,sp,caps,budget,depth+1); if(r!=0) return r; if(idx<2*(RE_MAXGROUP+1)) caps[idx]=old; return 0; }
            case I_MATCH: return 1;
        }
        return 0;
    }
}
/* search at or after `start`; fills caps[0..]=match+groups; returns match start or -1 */
static int re_search(regex *re,const char*s,int slen,int start,int*caps){
    if(!re||!re->ok||!re->prog) return -1;
    for(int sp=start; sp<=slen; sp++){
        for(int i=0;i<2*(RE_MAXGROUP+1);i++) caps[i]=-1;
        long budget=300000;
        if(re_run(re,0,s,slen,sp,caps,&budget,0)==1) return caps[0];
    }
    return -1;
}
/* build the [fullMatch, g1, g2, …] result array (with an .index property) from caps */
static val re_result(regex *re,const char*s,int*caps){
    obj *a=new_obj(V_ARR); if(!a){ g_oom=1; return UND(); }
    for(int g=0; g<=re->ngroup; g++){ int st=caps[2*g], en=caps[2*g+1];
        if(st>=0 && en>=st){ char*m=aalloc(en-st+1); if(m){ memcpy(m,s+st,en-st); m[en-st]=0; } arr_push_val(a, STRV(m?m:"")); }
        else arr_push_val(a, UND()); }
    /* note: JS exposes a .index on this array; arrays here can't carry named props, so it's omitted */
    val r=UND(); r.t=V_ARR; r.o=a; return r;
}
static regex *rx_of(val v){ return (v.t==V_OBJ && v.o && v.o->kind==V_REGEX) ? (regex*)v.o->rx : 0; }
/* a growable string builder on the arena (no realloc/free; grows by doubling) */
typedef struct { char *buf; int len, cap; } sbuild;
static void sb_put(sbuild *b, const char *s, int n){ if(n<0) return; if(b->len+n+1>b->cap){ int nc=b->cap*2; if(nc<b->len+n+16) nc=b->len+n+16; char*nb=aalloc(nc); if(!nb){g_oom=1;return;} if(b->buf) memcpy(nb,b->buf,b->len); b->buf=nb; b->cap=nc; } if(b->buf){ memcpy(b->buf+b->len,s,n); b->len+=n; } }
/* expand a replacement template ($&=whole match, $1..$9=group, $$=$) into b */
static void sb_expand(sbuild *b, const char *repl, const char *s, int *caps, int ngroup){
    int rl=(int)strlen(repl);
    for(int i=0;i<rl;i++){ if(repl[i]=='$' && i+1<rl){ char d=repl[i+1];
        if(d=='&'){ sb_put(b, s+caps[0], caps[1]-caps[0]); i++; continue; }
        if(d=='$'){ sb_put(b,"$",1); i++; continue; }
        if(d>='1'&&d<='9'){ int g=d-'0'; if(g<=ngroup){ int a=caps[2*g],e=caps[2*g+1]; if(a>=0&&e>=a) sb_put(b,s+a,e-a); } i++; continue; } }
        sb_put(b, repl+i, 1); }
}
/* methods on a RegExp object (recv.o->kind==V_REGEX, recv.o->rx is the regex*) */
static val eval_regex_method(val recv, const char *name, val *args, int nargs){
    regex *re=(regex*)recv.o->rx; if(!re){ return UND(); }
    const char *s = nargs? val_to_str(args[0]) : ""; int slen=(int)strlen(s);
    int caps[2*(RE_MAXGROUP+1)];
    if(strcmp(name,"test")==0){ int start = re->global ? re->lastIndex : 0; if(start<0||start>slen) start=0;
        int st=re_search(re,s,slen,start,caps);
        if(re->global) re->lastIndex = (st>=0) ? (caps[1]>caps[0]?caps[1]:caps[1]+1) : 0;
        return BOOLV(st>=0); }
    if(strcmp(name,"exec")==0){ int start = re->global ? re->lastIndex : 0; if(start<0||start>slen) start=0;
        int st=re_search(re,s,slen,start,caps);
        if(st<0){ if(re->global) re->lastIndex=0; val nv=UND(); nv.t=V_NULL; return nv; }
        if(re->global) re->lastIndex = caps[1]>caps[0]?caps[1]:caps[1]+1;
        return re_result(re,s,caps); }
    rt_err("unknown RegExp method"); return UND();
}
/* build a V_REGEX object from a pattern + flags string (shared by RegExp() and /literals/) */
static val make_regex_val(const char *pat, const char *fl){
    regex *re=re_compile(pat?pat:"", fl?fl:"");
    obj *o=new_obj(V_REGEX); if(!o){ g_oom=1; return UND(); }
    o->rx=re; obj_set(o,"source",STRV(pat?pat:"")); obj_set(o,"global",BOOLV(re&&re->global)); obj_set(o,"flags",STRV(fl?fl:""));
    return obj_val(o);
}
/* RegExp(pattern, flags) / new RegExp(...) -> a V_REGEX object */
static val nat_regexp(val *args, int nargs){
    const char *pat = nargs>0? val_to_str(args[0]) : "";
    const char *fl  = nargs>1? val_to_str(args[1]) : "";
    return make_regex_val(pat, fl);
}
static const char *val_to_str_inner(val v) {
    switch (v.t) {
        case V_UNDEF: return "undefined";
        case V_NULL: return "null";
        case V_BOOL: return v.num?"true":"false";
        case V_NUM: return i64_to_str(v.num);
        case V_STR: return v.str;
        case V_FUN: case V_NATIVE: return "function";
        case V_ARR: {
            /* two passes: stringify each element, sum the REAL lengths (a wrong
             * n*24 estimate would overflow the buffer for long element strings),
             * then allocate exactly. total is 64-bit so it can't overflow. */
            obj *o=v.o;
            const char **parts = aalloc((long)sizeof(char*) * (o->n>0?o->n:1));
            if (!parts) return "[array]";
            long total = 0;
            for (int i=0;i<o->n;i++){ parts[i]=val_to_str(o->vals[i]); total += (long)strlen(parts[i]) + 1; }
            char *buf=aalloc(total+1); if(!buf) return "[array]"; int p=0;
            for (int i=0;i<o->n;i++){ if(i) buf[p++]=','; const char*s=parts[i]; while(*s) buf[p++]=*s++; }
            buf[p]=0; return buf;
        }
        case V_OBJ: return "[object Object]";
    }
    return "";
}
/* depth-guarded wrapper: stops infinite recursion on a self-referential value
 * (e.g. `var a=[]; a.push(a); print(a);`) before it overruns the kernel stack. */
static const char *val_to_str(val v) {
    if (++g_depth > MAXDEPTH) { g_depth--; return "[...]"; }
    const char *s = val_to_str_inner(v); g_depth--; return s;
}

/* ---- environments ---- */
static env *new_env(env *parent){ env *e=aalloc(sizeof(env)); if(!e) return 0; e->cap=4; e->keys=aalloc(sizeof(char*)*e->cap); e->vals=aalloc(sizeof(val)*e->cap); if(!e->keys||!e->vals){ g_oom=1; return 0; } e->n=0; e->parent=parent; return e; }
static void env_define(env *e, const char *key, val v) {
    for (int i=0;i<e->n;i++) if (strcmp(e->keys[i],key)==0){ e->vals[i]=v; return; }
    if (e->n>=e->cap){ int nc=e->cap*2; const char**nk=aalloc(sizeof(char*)*nc); val*nv=aalloc(sizeof(val)*nc); if(!nk||!nv){g_oom=1;return;} memcpy(nk,e->keys,sizeof(char*)*e->n); memcpy(nv,e->vals,sizeof(val)*e->n); e->keys=nk; e->vals=nv; e->cap=nc; }
    e->keys[e->n]=key; e->vals[e->n]=v; e->n++;
}
static val *env_find(env *e, const char *key) {
    for (; e; e=e->parent) for (int i=0;i<e->n;i++) if (strcmp(e->keys[i],key)==0) return &e->vals[i];
    return 0;
}

/* =========================== evaluator =========================== */
enum { C_NORMAL, C_RETURN, C_BREAK, C_CONTINUE };
typedef struct { int kind; val v; } comp;

static val eval_expr(node *n, env *e);
static comp eval_stmt(node *n, env *e);
static void bind_pattern(node *pat, val v, env *e);   /* destructuring (defined below) */

static const char *node_name(node *n){ return n->str ? n->str : ""; }   /* names interned at parse time */

/* Call `fn` with an explicit `this` binding. Regular functions bind `this` in
 * their call frame (a method's receiver, the new object under `new`, or undefined
 * for a plain call); arrow functions (node->prefix==1) deliberately do NOT bind
 * one, so `this` resolves lexically up the scope chain to the enclosing function. */
static val call_function_this(val fn, val thisv, val *args, int nargs) {
    if (fn.t==V_NATIVE) return fn.o->native(args,nargs);
    if (fn.t!=V_FUN) { rt_err("not a function"); return UND(); }
    if (++g_depth > MAXDEPTH) { rt_err("max call depth"); g_depth--; return UND(); }
    env *fe = new_env(fn.o->scope);
    if (!fe) { g_oom=1; g_depth--; return UND(); }     /* arena exhausted: bail, don't deref NULL */
    node *def = fn.o->fn;
    if (!def->prefix) {                                /* non-arrow gets its own `this` (+ `super` if a class member) */
        env_define(fe, "this", thisv);
        if (fn.o->super_class) { val sup=UND(); sup.t=V_FUN; sup.o=fn.o->super_class; env_define(fe, "@super", sup); }
    }
    for (int i=0;i<def->nlist;i++){ node *pn=def->list[i];
        if (pn->type!=N_IDENT) {   /* destructuring param ([a,b], {c}, or =default wrapping a pattern) */
            bind_pattern(pn, (i<nargs)?args[i]:UND(), fe); continue;
        }
        if (pn->op=='.') {   /* ...rest: gather the remaining args into an array, then stop */
            obj *ro=new_obj(V_ARR); if(!ro){ g_oom=1; break; }
            for (int j=i;j<nargs && !g_oom;j++) arr_push_val(ro, args[j]);
            val rv=UND(); rv.t=V_ARR; rv.o=ro; env_define(fe, node_name(pn), rv); break;
        }
        val pv = (i<nargs) ? args[i] : (pn->a ? eval_expr(pn->a, fe) : UND());   /* default value if arg omitted */
        env_define(fe, node_name(pn), pv); }
    comp c = eval_stmt(def->a, fe);
    g_depth--;
    return c.kind==C_RETURN ? c.v : UND();
}
static val call_function(val fn, val *args, int nargs){ return call_function_this(fn, UND(), args, nargs); }

/* Evaluate call-argument nodes into a flat array, expanding `...spread` of arrays
 * (and strings → chars). Returns the count, capped at maxargs. */
static int build_args(node **list, int nlist, env *e, val *args, int maxargs) {
    int na=0;
    for (int i=0;i<nlist && na<maxargs;i++){
        node *el=list[i];
        if (el->type==N_SPREAD){
            val v=eval_expr(el->a,e);
            if (v.t==V_ARR && v.o){ for(int j=0;j<v.o->n && na<maxargs;j++) args[na++]=v.o->vals[j]; }
            else if (v.t==V_STR){ const char*s=v.str; for(int j=0;s[j] && na<maxargs;j++){ char*c=aalloc(2); if(c){c[0]=s[j];c[1]=0;} args[na++]=STRV(c?c:""); } }
            /* a non-iterable spread contributes nothing */
        } else { args[na++]=eval_expr(el,e); }
    }
    return na;
}

/* Bind a destructuring pattern (an N_ARRAY/N_OBJECT of targets, reusing the
 * literal parsers) to a value, defining each leaf identifier in `e`. Handles
 * defaults (N_ASSIGN), array/object rest (N_SPREAD), rename, and nesting.
 * Pattern nesting is already bounded by the parser's depth guard, but bind_pattern
 * carries its own g_depth guard too (via the wrapper below) so it can never be the
 * path that overflows the C stack regardless of the entry task's stack budget. */
static void bind_pat(node *pat, val v, env *e, int assign);   /* guarded wrapper (fwd) */
static void bind_pat_inner(node *pat, val v, env *e, int assign) {
    if (!pat || g_oom) return;
    if (pat->type==N_ASSIGN) { if (v.t==V_UNDEF) v=eval_expr(pat->b,e); bind_pat(pat->a, v, e, assign); return; }
    if (pat->type==N_IDENT || pat->type==N_MEMBER || pat->type==N_INDEX) {   /* a leaf target */
        if (!assign) { env_define(e, node_name(pat), v); return; }           /* var/param/for-of: fresh binding */
        /* assignment target — match N_ASSIGN's identifier/member/index semantics */
        if (pat->type==N_IDENT) { const char *nm=node_name(pat); val *slot=env_find(e,nm); if(slot) *slot=v; else env_define(e,nm,v); }
        else if (pat->type==N_MEMBER) { val recv=eval_expr(pat->a,e); if((recv.t==V_OBJ||recv.t==V_ARR)&&recv.o) obj_set(recv.o, node_name(pat), v); }
        else { val recv=eval_expr(pat->a,e), idx=eval_expr(pat->b,e);
               if (recv.t==V_ARR && recv.o){ int i=(int)to_num(idx); if(i>=0&&i<recv.o->n) recv.o->vals[i]=v; }
               else if (recv.t==V_OBJ && recv.o) obj_set(recv.o, val_to_str(idx), v); }
        return;
    }
    if (pat->type==N_ARRAY) {
        for (int i=0;i<pat->nlist && !g_oom;i++){ node *el=pat->list[i];
            if (el->type==N_SPREAD){ obj *ro=new_obj(V_ARR); if(!ro){g_oom=1;return;}
                if (v.t==V_ARR && v.o) for(int j=i;j<v.o->n && !g_oom;j++) arr_push_val(ro, v.o->vals[j]);
                val rv=UND(); rv.t=V_ARR; rv.o=ro; bind_pat(el->a, rv, e, assign); break; }
            val ev = (v.t==V_ARR && v.o && i<v.o->n) ? v.o->vals[i] : UND();
            bind_pat(el, ev, e, assign);
        }
        return;
    }
    if (pat->type==N_OBJECT) {
        for (int i=0;i<pat->nlist && !g_oom;i++){ node *pr=pat->list[i];
            if (pr->type==N_SPREAD){ obj *ro=new_obj(V_OBJ); if(!ro){g_oom=1;return;}
                if (v.t==V_OBJ && v.o) for(int j=0;j<v.o->n && !g_oom;j++){ const char*k=v.o->keys[j]; int named=0;
                    for(int m=0;m<i;m++){ node*q=pat->list[m]; if(q->type!=N_SPREAD && q->str && strcmp(q->str,k)==0){named=1;break;} }
                    if(!named) obj_set(ro, k, v.o->vals[j]); }
                val rv=UND(); rv.t=V_OBJ; rv.o=ro; bind_pat(pr->a, rv, e, assign); break; }
            val ev=UND(); if (v.t==V_OBJ && v.o) obj_get(v.o, pr->str, &ev);
            bind_pat(pr->a, ev, e, assign);   /* pr->a is the target (ident, N_ASSIGN default, member, or nested pattern) */
        }
        return;
    }
}
static void bind_pat(node *pat, val v, env *e, int assign) {
    if (++g_depth > MAXDEPTH) { g_depth--; rt_err("pattern too deeply nested"); return; }
    bind_pat_inner(pat, v, e, assign); g_depth--;
}
static void bind_pattern(node *pat, val v, env *e)        { bind_pat(pat, v, e, 0); }   /* var / param / for-of (declarations) */
static void bind_pattern_assign(node *pat, val v, env *e) { bind_pat(pat, v, e, 1); }   /* [a,b]=… / ({x}=…) (assignments) */

/* resolve a member/index target for assignment: returns the container + key */
static val eval_string_method(val recv, const char *name, val *args, int nargs);
static val eval_array_method(val recv, const char *name, val *args, int nargs);
static val eval_number_method(val recv, const char *name, val *args, int nargs);
static val eval_map_method(val recv, const char *name, val *args, int nargs);
static val eval_set_method(val recv, const char *name, val *args, int nargs);

static val eval_member_get(val recv, const char *name) {
    if (recv.t==V_STR) { if (strcmp(name,"length")==0) return NUM((int64_t)strlen(recv.str)); }
    if (recv.t==V_ARR && recv.o) {        /* recv.o can be NULL if a producing method hit OOM */
        if (strcmp(name,"length")==0) return NUM(recv.o->n);
        /* arrays store elements in vals[] with keys[] unused — no named-property lookup here */
    }
    if (recv.t==V_OBJ && recv.o) {
        if (recv.o->kind==V_MAP && strcmp(name,"size")==0) return NUM(recv.o->n/2);   /* entries are [k,v] pairs */
        if (recv.o->kind==V_SET && strcmp(name,"size")==0) return NUM(recv.o->n);
        val out; if (obj_get(recv.o,name,&out)) return out;
    }
    return UND();
}

static val eval_expr_inner(node *n, env *e) {
    if (g_err || g_oom) return UND();
    switch (n->type) {
        case N_NUM: return NUM(n->num);
        case N_STR: return STRV(n->str);
        case N_REGEX: { char fl[3]; int k=0; if(n->num&1)fl[k++]='g'; if(n->num&2)fl[k++]='i'; fl[k]=0; return make_regex_val(node_name(n), intern(fl,k)); }   /* intern: flags must be arena-stable, not a dead stack buffer */
        case N_BOOL: return BOOLV((int)n->num);
        case N_NULL: { val v=UND(); v.t=V_NULL; return v; }
        case N_UNDEF: return UND();
        case N_IDENT: { const char *nm=node_name(n); val *p=env_find(e,nm); if(!p){ rt_err("undefined variable"); return UND(); } return *p; }
        case N_ARRAY: { obj *o=new_obj(V_ARR); if(!o) return UND();
            for(int i=0;i<n->nlist && !g_oom;i++){ node *el=n->list[i];
                if (el->type==N_SPREAD){ val sv=eval_expr(el->a,e);
                    if (sv.t==V_ARR && sv.o){ for(int j=0;j<sv.o->n && !g_oom;j++) arr_push_val(o, sv.o->vals[j]); }
                    else if (sv.t==V_STR){ const char*s=sv.str; for(int j=0;s[j] && !g_oom;j++){ char*c=aalloc(2); if(c){c[0]=s[j];c[1]=0;} arr_push_val(o, STRV(c?c:"")); } }
                } else arr_push_val(o, eval_expr(el,e));
            }
            val r=UND(); r.t=V_ARR; r.o=o; return r; }
        case N_OBJECT: { obj *o=new_obj(V_OBJ); if(!o) return UND();
            for(int i=0;i<n->nlist && !g_oom;i++){ node*pr=n->list[i];
                if (pr->type==N_SPREAD){ val sv=eval_expr(pr->a,e);
                    if (sv.t==V_OBJ && sv.o){ for(int j=0;j<sv.o->n && !g_oom;j++) obj_set(o, sv.o->keys[j], sv.o->vals[j]); }
                } else { const char *key = pr->b ? val_to_str(eval_expr(pr->b,e)) : node_name(pr);   /* pr->b = computed key */
                    obj_set(o, key, eval_expr(pr->a,e)); }
            }
            val r=UND(); r.t=V_OBJ; r.o=o; return r; }
        case N_FUNC: { obj *o=new_obj(V_FUN); if(!o) return UND(); o->fn=n; o->scope=e; val r=UND(); r.t=V_FUN; r.o=o; if(n->str){ env_define(e,node_name(n),r); } return r; }
        case N_COND: return truthy(eval_expr(n->a,e)) ? eval_expr(n->b,e) : eval_expr(n->c,e);
        case N_LOGICAL: { val l=eval_expr(n->a,e);
            if(n->op=='N') return (l.t==V_UNDEF||l.t==V_NULL) ? eval_expr(n->b,e) : l;   /* ?? : only null/undefined fall through */
            if(n->op=='A') return truthy(l)?eval_expr(n->b,e):l; else return truthy(l)?l:eval_expr(n->b,e); }
        case N_UNARY: {
            if (n->op=='t') { val v=eval_expr(n->a,e); const char*ty= v.t==V_UNDEF?"undefined":v.t==V_NULL?"object":v.t==V_BOOL?"boolean":v.t==V_NUM?"number":v.t==V_STR?"string":(v.t==V_FUN||v.t==V_NATIVE)?"function":"object"; return STRV(ty); }
            val v=eval_expr(n->a,e);
            if (n->op=='!') return BOOLV(!truthy(v));
            if (n->op=='-') return NUM(-to_num(v));
            if (n->op=='+') return NUM(to_num(v));
            return UND();
        }
        case N_UPDATE: {
            node *t=n->a; val *slot=0;
            if (t->type==N_IDENT) { slot=env_find(e,node_name(t)); }
            else if (t->type==N_MEMBER) {            /* o.prop++ */
                val recv=eval_expr(t->a,e);
                if (recv.t==V_OBJ && recv.o) { const char *key=node_name(t);
                    for (int i=0;i<recv.o->n;i++) if(strcmp(recv.o->keys[i],key)==0){ slot=&recv.o->vals[i]; break; }
                    if (!slot) { obj_set(recv.o,key,NUM(0)); for (int i=0;i<recv.o->n;i++) if(strcmp(recv.o->keys[i],key)==0){ slot=&recv.o->vals[i]; break; } }
                }
            }
            else if (t->type==N_INDEX) {             /* arr[i]++ / o[k]++ */
                val recv=eval_expr(t->a,e), idx=eval_expr(t->b,e);
                if (recv.t==V_ARR && recv.o) { int i=(int)to_num(idx); if(i>=0&&i<recv.o->n) slot=&recv.o->vals[i]; }
                else if (recv.t==V_OBJ && recv.o) { const char *key=val_to_str(idx);
                    for (int i=0;i<recv.o->n;i++) if(strcmp(recv.o->keys[i],key)==0){ slot=&recv.o->vals[i]; break; }
                    if (!slot) { obj_set(recv.o,key,NUM(0)); for (int i=0;i<recv.o->n;i++) if(strcmp(recv.o->keys[i],key)==0){ slot=&recv.o->vals[i]; break; } }
                }
            }
            if (!slot) { rt_err("invalid ++/-- target"); return UND(); }
            int64_t old=to_num(*slot); int64_t nw = n->op=='+'?old+1:old-1; *slot=NUM(nw);
            return NUM(n->prefix?nw:old);
        }
        case N_BINARY: {
            val a=eval_expr(n->a,e), b=eval_expr(n->b,e);
            if (n->op=='+') { if (a.t==V_STR||b.t==V_STR) { const char*sa=val_to_str(a),*sb=val_to_str(b); int la=(int)strlen(sa),lb=(int)strlen(sb); char*s=aalloc(la+lb+1); if(!s) return UND(); memcpy(s,sa,la); memcpy(s+la,sb,lb); s[la+lb]=0; return STRV(s); } return NUM(to_num(a)+to_num(b)); }
            int64_t x=to_num(a), y=to_num(b);
            switch (n->op) {
                case '-': return NUM(x-y);
                case '*': return NUM(x*y);
                case '/': return NUM(y?x/y:0);
                case '%': return NUM(y?x%y:0);
                case '<': return BOOLV(x<y); case '>': return BOOLV(x>y);
                case 'l': return BOOLV(x<=y); case 'g': return BOOLV(x>=y);
                case '&': return NUM(x&y); case '|': return NUM(x|y);
                case 'L': return NUM((int64_t)((uint64_t)x<<(y&63))); case 'R': return NUM(x>>(y&63));
                case '=': {
                    if (a.t==V_STR&&b.t==V_STR) return BOOLV(strcmp(a.str,b.str)==0);
                    if (a.t!=b.t && !((a.t==V_NUM||a.t==V_BOOL)&&(b.t==V_NUM||b.t==V_BOOL))) return BOOLV(0);
                    return BOOLV(x==y);
                }
                case '!': {
                    if (a.t==V_STR&&b.t==V_STR) return BOOLV(strcmp(a.str,b.str)!=0);
                    if (a.t!=b.t && !((a.t==V_NUM||a.t==V_BOOL)&&(b.t==V_NUM||b.t==V_BOOL))) return BOOLV(1);
                    return BOOLV(x!=y);
                }
            }
            return UND();
        }
        case N_ASSIGN: {
            val rhs = eval_expr(n->b,e);
            node *t=n->a;
            if (n->op!='=') { val cur=eval_expr(t,e); int64_t x=to_num(cur),y=to_num(rhs);
                if (n->op=='+'&&(cur.t==V_STR||rhs.t==V_STR)) { const char*sa=val_to_str(cur),*sb=val_to_str(rhs); int la=(int)strlen(sa),lb=(int)strlen(sb); char*s=aalloc(la+lb+1); if(s){memcpy(s,sa,la);memcpy(s+la,sb,lb);s[la+lb]=0;} rhs=STRV(s?s:""); }
                else rhs = NUM(n->op=='+'?x+y: n->op=='-'?x-y: n->op=='*'?x*y: n->op=='/'?(y?x/y:0): (y?x%y:0)); }
            if ((t->type==N_ARRAY || t->type==N_OBJECT) && n->op=='=') { bind_pattern_assign(t, rhs, e); return rhs; }   /* [a,b]=… / ({x}=…) */
            if (t->type==N_IDENT) { const char*nm=node_name(t); val *slot=env_find(e,nm); if(slot) *slot=rhs; else env_define(e,nm,rhs); return rhs; }
            if (t->type==N_MEMBER) { val recv=eval_expr(t->a,e); if((recv.t==V_OBJ||recv.t==V_ARR)&&recv.o){ obj_set(recv.o, node_name(t), rhs); } return rhs; }
            if (t->type==N_INDEX) { val recv=eval_expr(t->a,e); val idx=eval_expr(t->b,e);
                if (recv.t==V_ARR && recv.o) {
                    long i = to_num(idx);
                    if (i < 0 || i > (1<<24)) { rt_err("array index out of range"); return rhs; }
                    while (recv.o->n <= (int)i && !g_oom) {           /* grow with UND() to reach i */
                        if (recv.o->n >= recv.o->cap) {
                            int nc = recv.o->cap*2; if (nc <= (int)i) nc = (int)i + 1;   /* i<=2^24: no int overflow */
                            val *nv = aalloc((long)sizeof(val)*nc);   /* 64-bit size; rejects if > arena */
                            if (!nv) break;                            /* g_oom set; do NOT write below */
                            memcpy(nv, recv.o->vals, sizeof(val)*recv.o->n); recv.o->vals=nv; recv.o->cap=nc;
                        }
                        recv.o->vals[recv.o->n++]=UND();
                    }
                    if (!g_oom && (int)i < recv.o->n) recv.o->vals[(int)i]=rhs;
                }
                else if (recv.t==V_OBJ && recv.o) { obj_set(recv.o, val_to_str(idx), rhs); }
                return rhs; }
            rt_err("invalid assignment target"); return UND();
        }
        case N_SUPER: { val *s=env_find(e,"@super"); return s?*s:UND(); }
        case N_MEMBER: {
            if (n->a->type==N_SUPER) {   /* super.prop (no call): read from the parent's method table */
                val *sup=env_find(e,"@super");
                if (sup && sup->t==V_FUN && sup->o->home_proto){ val out; if(obj_get(sup->o->home_proto,node_name(n),&out)) return out; }
                return UND();
            }
            val recv=eval_expr(n->a,e);
            if (n->prefix && (recv.t==V_UNDEF||recv.t==V_NULL)) return UND();   /* obj?.prop short-circuit */
            return eval_member_get(recv, node_name(n)); }
        case N_INDEX: { val recv=eval_expr(n->a,e);
            if (n->prefix && (recv.t==V_UNDEF||recv.t==V_NULL)) return UND();   /* obj?.[i] short-circuit */
            val idx=eval_expr(n->b,e);
            if (recv.t==V_ARR && recv.o){ int i=(int)to_num(idx); if(i>=0&&i<recv.o->n) return recv.o->vals[i]; return UND(); }
            if (recv.t==V_STR){ int i=(int)to_num(idx); int l=(int)strlen(recv.str); if(i>=0&&i<l){ char*s=aalloc(2); if(s){s[0]=recv.str[i]; s[1]=0;} return STRV(s?s:"");} return UND(); }
            if (recv.t==V_OBJ && recv.o){ val out; if(obj_get(recv.o,val_to_str(idx),&out)) return out; }
            return UND(); }
        case N_CALL: {
            /* method call a.b(...) needs the receiver for string/array methods */
            node *callee=n->a; val args[16]; int na=build_args(n->list, n->nlist, e, args, 16);
            /* super(...) and super.m(...): resolve via the call frame's @super (the
             * parent constructor), invoked with the current `this`. */
            if (callee->type==N_SUPER) {
                val *sup=env_find(e,"@super"), *th=env_find(e,"this");
                if (!sup || sup->t!=V_FUN) { rt_err("super outside a derived constructor"); return UND(); }
                call_function_this(*sup, th?*th:UND(), args, na); return UND();
            }
            if (callee->type==N_MEMBER && callee->a->type==N_SUPER) {
                val *sup=env_find(e,"@super"), *th=env_find(e,"this"); const char *m=node_name(callee);
                if (!sup || sup->t!=V_FUN || !sup->o->home_proto) { rt_err("no super method"); return UND(); }
                val fn; if (obj_get(sup->o->home_proto,m,&fn)) return call_function_this(fn, th?*th:UND(), args, na);
                rt_err("no such super method"); return UND();
            }
            if (callee->type==N_MEMBER) {
                val recv=eval_expr(callee->a,e); const char *m=node_name(callee);
                if (callee->prefix && (recv.t==V_UNDEF||recv.t==V_NULL)) return UND();   /* obj?.method() short-circuit */
                if (recv.t==V_STR) return eval_string_method(recv,m,args,na);
                if (recv.t==V_ARR) return eval_array_method(recv,m,args,na);
                if (recv.t==V_NUM || recv.t==V_BOOL) return eval_number_method(recv,m,args,na);
                if (recv.t==V_OBJ && recv.o && recv.o->kind==V_MAP) return eval_map_method(recv,m,args,na);
                if (recv.t==V_OBJ && recv.o && recv.o->kind==V_SET) return eval_set_method(recv,m,args,na);
                if (recv.t==V_OBJ && recv.o && recv.o->kind==V_REGEX) return eval_regex_method(recv,m,args,na);
                if (recv.t==V_OBJ && recv.o) { val fn; if(obj_get(recv.o,m,&fn)){ if(n->prefix && (fn.t==V_UNDEF||fn.t==V_NULL)) return UND(); return call_function_this(fn,recv,args,na); } }
                if (n->prefix) return UND();   /* obj.method?.() where method is absent */
                rt_err("no such method"); return UND();
            }
            val fn=eval_expr(callee,e);
            if (n->prefix && (fn.t==V_UNDEF||fn.t==V_NULL)) return UND();   /* fn?.() short-circuit */
            return call_function(fn,args,na);
        }
        case N_THIS: { val *t=env_find(e,"this"); return t?*t:UND(); }
        case N_CLASS: {
            /* Build the method table P, then a constructor function value carrying
             * P in home_proto. `extends` copies the parent's methods into P first
             * (children override by being added after), and an absent child
             * constructor inherits the parent's. Each own method/ctor records its
             * parent constructor in super_class so super()/super.m() can find it. */
            obj *P=new_obj(V_OBJ); if(!P){ g_oom=1; return UND(); }
            node *ctor_node=0; val parentC=UND(); int has_parent=0; obj *parent_obj=0;
            if (n->a) {
                parentC=eval_expr(n->a,e); has_parent=(parentC.t==V_FUN); if(has_parent) parent_obj=parentC.o;
                if (has_parent && parentC.o->home_proto) { obj *pp=parentC.o->home_proto; for(int i=0;i<pp->n && !g_oom;i++) obj_set(P,pp->keys[i],pp->vals[i]); }
            }
            for (int i=0;i<n->nlist && !g_oom;i++){ node *m=n->list[i];
                if (strcmp(node_name(m),"constructor")==0){ ctor_node=m; continue; }
                obj *fo=new_obj(V_FUN); if(!fo){ g_oom=1; return UND(); } fo->fn=m; fo->scope=e; fo->super_class=parent_obj;
                val fv=UND(); fv.t=V_FUN; fv.o=fo; obj_set(P, node_name(m), fv);
            }
            obj *ctor_super=parent_obj;   /* own ctor: super is this class's parent */
            if (!ctor_node && has_parent) { ctor_node = parentC.o->fn; ctor_super = parentC.o->super_class; }  /* inherited ctor: its super is the GRANDparent (where it was defined) */
            if (!ctor_node) { node *em=mknode(N_FUNC); em->list=aalloc(sizeof(node*)); em->nlist=0; em->a=mknode(N_BLOCK); ctor_node=em; }
            obj *co=new_obj(V_FUN); if(!co){ g_oom=1; return UND(); } co->fn=ctor_node; co->scope=e; co->home_proto=P; co->super_class=ctor_super;
            val cv=UND(); cv.t=V_FUN; cv.o=co;
            if (n->str) env_define(e, node_name(n), cv);
            return cv;
        }
        case N_NEW: {
            val ctor=eval_expr(n->a,e); val args[16]; int na=build_args(n->list, n->nlist, e, args, 16);
            if (ctor.t==V_NATIVE) return ctor.o->native(args,na);   /* new Map() / new Set(): native makes the instance */
            if (ctor.t!=V_FUN) { rt_err("not a constructor"); return UND(); }
            obj *self=new_obj(V_OBJ); if(!self){ g_oom=1; return UND(); }
            /* class instance: copy the class's methods onto the new object as own
             * properties (we model methods by copying rather than a prototype chain) */
            if (ctor.o->home_proto) { obj *P=ctor.o->home_proto; for(int i=0;i<P->n && !g_oom;i++) obj_set(self,P->keys[i],P->vals[i]); }
            val selfv=obj_val(self);
            val r=call_function_this(ctor, selfv, args, na);
            /* a constructor that explicitly returns an object overrides `this` */
            return (r.t==V_OBJ||r.t==V_ARR) ? r : selfv;
        }
    }
    return UND();
}

/* depth-guarded wrapper around the expression evaluator: a deep AST (member/index
 * chains, nested operators, self-referential structures via val_to_str) would
 * otherwise recurse C-stack-deep. Shares g_depth with the parser/calls. */
static val eval_expr(node *n, env *e) {
    if (++g_depth > MAXDEPTH) { rt_err("expression too deeply nested"); g_depth--; return UND(); }
    val v = eval_expr_inner(n, e); g_depth--; return v;
}

static comp CN(void){ comp c; c.kind=C_NORMAL; c.v=UND(); return c; }

static comp eval_stmt_inner(node *n, env *e) {
    if (g_err || g_oom) return CN();
    switch (n->type) {
        case N_PROGRAM: case N_BLOCK: {
            env *be = (n->type==N_BLOCK)? new_env(e) : e;
            if (!be) { g_oom=1; return CN(); }
            for (int i=0;i<n->nlist;i++){ comp c=eval_stmt(n->list[i],be); if(c.kind!=C_NORMAL||g_err||g_oom) return c; }
            return CN();
        }
        case N_VAR: { for(int i=0;i<n->nlist;i++){ node*d=n->list[i]; val v = d->a?eval_expr(d->a,e):UND();
            if (d->b) bind_pattern(d->b, v, e); else env_define(e,node_name(d),v); } return CN(); }
        case N_FUNC: { eval_expr(n,e); return CN(); }
        case N_EXPR: { eval_expr(n->a,e); return CN(); }
        case N_IF: { if (truthy(eval_expr(n->a,e))) return eval_stmt(n->b,e); else if (n->c) return eval_stmt(n->c,e); return CN(); }
        case N_WHILE: {
            int guard=0;
            while (truthy(eval_expr(n->a,e))) { if(++guard>5000000){rt_err("loop limit");break;} comp c=eval_stmt(n->b,e); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; if(g_err||g_oom) break; }
            return CN();
        }
        case N_DOWHILE: {
            int guard=0;
            do { comp c=eval_stmt(n->b,e); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; if(g_err||g_oom) break; if(++guard>5000000){rt_err("loop limit");break;} } while (truthy(eval_expr(n->a,e)));
            return CN();
        }
        case N_SWITCH: {
            val disc=eval_expr(n->a,e); env *se=new_env(e); if(!se){ g_oom=1; return CN(); }
            int matched=-1, defidx=-1;
            for (int i=0;i<n->nlist && !g_err && !g_oom;i++){ node*cl=n->list[i]; if(!cl->a){ defidx=i; continue; }
                val cv=eval_expr(cl->a,se); int eq;
                if (disc.t==V_STR && cv.t==V_STR) eq=(strcmp(disc.str,cv.str)==0);
                else if ((disc.t==V_NUM||disc.t==V_BOOL)&&(cv.t==V_NUM||cv.t==V_BOOL)) eq=(disc.num==cv.num);
                else eq=(disc.t==cv.t && disc.num==cv.num);
                if (eq){ matched=i; break; } }
            if (matched<0) matched=defidx;
            if (matched>=0) for (int i=matched;i<n->nlist;i++){ node*cl=n->list[i];
                for (int j=0;j<cl->nlist;j++){ comp c=eval_stmt(cl->list[j],se);
                    if (c.kind==C_BREAK) return CN(); if (c.kind==C_RETURN||c.kind==C_CONTINUE) return c; if (g_err||g_oom) return CN(); } }
            return CN();
        }
        case N_FOR: {
            env *fe=new_env(e); if(!fe){ g_oom=1; return CN(); }
            if(n->a){ if(n->a->type==N_VAR) eval_stmt(n->a,fe); else eval_expr(n->a,fe); }
            int guard=0;
            while (!n->b || truthy(eval_expr(n->b,fe))) {
                if(++guard>5000000){rt_err("loop limit");break;}
                comp c=eval_stmt(n->d,fe); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; if(g_err) break;
                if(n->c) eval_expr(n->c,fe);
            }
            return CN();
        }
        case N_FOROF: {
            val it=eval_expr(n->a,e); env *fe=new_env(e); if(!fe){ g_oom=1; return CN(); }
            const char *vn = n->c ? 0 : node_name(n);   /* n->c is a destructuring pattern, else a plain name */
            if (vn) env_define(fe, vn, UND());
            if (it.t==V_ARR && it.o) {
                for (int i=0;i<it.o->n && !g_err && !g_oom;i++){
                    if (n->c) bind_pattern(n->c, it.o->vals[i], fe); else { val *slot=env_find(fe,vn); if(slot) *slot=it.o->vals[i]; }
                    comp c=eval_stmt(n->b,fe); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; }
            } else if (it.t==V_STR) {
                int l=(int)strlen(it.str);
                for (int i=0;i<l && !g_err && !g_oom;i++){ char*ch=aalloc(2); if(ch){ch[0]=it.str[i];ch[1]=0;} val cv=STRV(ch?ch:"");
                    if (n->c) bind_pattern(n->c, cv, fe); else { val *slot=env_find(fe,vn); if(slot) *slot=cv; }
                    comp c=eval_stmt(n->b,fe); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; }
            } else if (it.t==V_OBJ && it.o && it.o->kind==V_SET) {
                for (int i=0;i<it.o->n && !g_err && !g_oom;i++){ val cv=it.o->vals[i];
                    if (n->c) bind_pattern(n->c, cv, fe); else { val *slot=env_find(fe,vn); if(slot) *slot=cv; }
                    comp c=eval_stmt(n->b,fe); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; }
            } else if (it.t==V_OBJ && it.o && it.o->kind==V_MAP) {
                for (int i=0;i+1<it.o->n && !g_err && !g_oom;i+=2){             /* each entry is a fresh [k,v] array */
                    obj *pair=new_obj(V_ARR); if(!pair){ g_oom=1; break; } arr_push_val(pair,it.o->vals[i]); arr_push_val(pair,it.o->vals[i+1]);
                    val cv=UND(); cv.t=V_ARR; cv.o=pair;
                    if (n->c) bind_pattern(n->c, cv, fe); else { val *slot=env_find(fe,vn); if(slot) *slot=cv; }
                    comp c=eval_stmt(n->b,fe); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; }
            }
            return CN();
        }
        case N_THROW: { val v=eval_expr(n->a,e);
            if(!g_err){ g_throwval=v; g_threw=1; g_err=1; const char*s=val_to_str(v); int i=0; while(s[i]&&i<127){g_errmsg[i]=s[i];i++;} g_errmsg[i]=0; }
            return CN(); }   /* unwinds via g_err; if the operand itself errored, that error propagates */
        case N_TRY: {
            comp tc = eval_stmt(n->a, e);                /* try block */
            if (g_err && !g_oom && n->b) {               /* caught — ONLY if a catch clause exists */
                /* copy the message into the arena BEFORE clearing g_errmsg (STRV stores the pointer) */
                val ev = g_threw ? g_throwval : STRV(intern(g_errmsg[0]?g_errmsg:"error", (int)strlen(g_errmsg[0]?g_errmsg:"error")));
                g_err=0; g_threw=0; g_errmsg[0]=0; tc=CN();
                env *ce=new_env(e); if(!ce){ g_oom=1; } else { if(n->str) env_define(ce, node_name(n), ev); tc=eval_stmt(n->b,ce); }
            }
            /* with no catch clause, g_err stays set so the exception propagates (re-raised
             * past finally via the save/restore below, or out of the try if no finally). */
            if (n->c) {                                  /* finally always runs (on a clean slate) */
                int s_err=g_err, s_threw=g_threw; val s_tv=g_throwval; char s_msg[128]; memcpy(s_msg,g_errmsg,128);
                g_err=0; g_threw=0;
                comp fc=eval_stmt(n->c,e);
                if (fc.kind!=C_NORMAL) return fc;        /* finally's return/break/continue wins */
                if (g_err) return tc;                    /* finally itself threw -> propagate it */
                g_err=s_err; g_threw=s_threw; g_throwval=s_tv; memcpy(g_errmsg,s_msg,128);   /* restore pending */
            }
            return tc;
        }
        case N_FORIN: {
            val it=eval_expr(n->a,e); env *fe=new_env(e); if(!fe){ g_oom=1; return CN(); }
            const char *vn=node_name(n); env_define(fe, vn, UND());
            if (it.t==V_OBJ && it.o) {
                for (int i=0;i<it.o->n && !g_err && !g_oom;i++){ val *slot=env_find(fe,vn); if(slot) *slot=STRV(it.o->keys[i]);
                    comp c=eval_stmt(n->b,fe); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; }
            } else if (it.t==V_ARR && it.o) {
                for (int i=0;i<it.o->n && !g_err && !g_oom;i++){ val *slot=env_find(fe,vn); if(slot) *slot=NUM(i);   /* array indices */
                    comp c=eval_stmt(n->b,fe); if(c.kind==C_BREAK) break; if(c.kind==C_RETURN) return c; }
            }
            return CN();
        }
        case N_RETURN: { comp c; c.kind=C_RETURN; c.v = n->a?eval_expr(n->a,e):UND(); return c; }
        case N_BREAK: { comp c=CN(); c.kind=C_BREAK; return c; }
        case N_CONTINUE: { comp c=CN(); c.kind=C_CONTINUE; return c; }
        default: eval_expr(n,e); return CN();
    }
}
/* depth-guarded wrapper around the statement evaluator (deeply nested blocks /
 * if / while / for). Shares g_depth with eval_expr, the parser, and calls. */
static comp eval_stmt(node *n, env *e) {
    if (++g_depth > MAXDEPTH) { rt_err("statements too deeply nested"); g_depth--; return CN(); }
    comp c = eval_stmt_inner(n, e); g_depth--; return c;
}

/* ---- builtin methods + globals ---- */
/* methods on number/bool primitives: (255).toString(16) -> "ff". The kernel is
 * integer-only (no FPU), so toFixed just renders the integer (decimals dropped). */
static val eval_number_method(val recv, const char *name, val *args, int nargs) {
    long long v=(long long)to_num(recv);
    if (strcmp(name,"toString")==0) {
        int radix=nargs?(int)to_num(args[0]):10; if(radix<2||radix>36) radix=10;
        char tmp[72]; int i=0; int neg=v<0;
        unsigned long long u = neg ? (unsigned long long)(-(v+1))+1ULL : (unsigned long long)v;
        if(u==0) tmp[i++]='0';
        while(u){ int d=(int)(u%(unsigned)radix); tmp[i++]=d<10?('0'+d):('a'+d-10); u/=(unsigned)radix; }
        if(neg) tmp[i++]='-';
        char*r=aalloc(i+1); if(!r) return STRV("");
        for(int j=0;j<i;j++){ r[j]=tmp[i-1-j]; } r[i]=0; return STRV(r);
    }
    if (strcmp(name,"toFixed")==0) return STRV(val_to_str(recv));   /* integer: no fractional part */
    if (strcmp(name,"valueOf")==0) return recv;
    rt_err("no such number method"); return UND();
}

static val eval_string_method(val recv, const char *name, val *args, int nargs) {
    const char *s=recv.str; int len=(int)strlen(s);
    if (strcmp(name,"charAt")==0){ int i=nargs?(int)to_num(args[0]):0; if(i<0||i>=len) return STRV(""); char*r=aalloc(2); r[0]=s[i]; r[1]=0; return STRV(r); }
    if (strcmp(name,"charCodeAt")==0){ int i=nargs?(int)to_num(args[0]):0; if(i<0||i>=len) return UND(); return NUM((unsigned char)s[i]); }
    if (strcmp(name,"toUpperCase")==0){ char*r=aalloc(len+1); for(int i=0;i<len;i++) r[i]=(s[i]>='a'&&s[i]<='z')?s[i]-32:s[i]; r[len]=0; return STRV(r); }
    if (strcmp(name,"toLowerCase")==0){ char*r=aalloc(len+1); for(int i=0;i<len;i++) r[i]=(s[i]>='A'&&s[i]<='Z')?s[i]+32:s[i]; r[len]=0; return STRV(r); }
    if (strcmp(name,"substring")==0||strcmp(name,"slice")==0){ int a=nargs>0?(int)to_num(args[0]):0; int b=nargs>1?(int)to_num(args[1]):len; if(a<0)a=0; if(b>len)b=len; if(b<a)b=a; char*r=aalloc(b-a+1); memcpy(r,s+a,b-a); r[b-a]=0; return STRV(r); }
    if (strcmp(name,"indexOf")==0){ if(!nargs) return NUM(-1); const char*sub=val_to_str(args[0]); int sl=(int)strlen(sub); for(int i=0;i+sl<=len;i++){ if(memcmp(s+i,sub,sl)==0) return NUM(i);} return NUM(-1); }
    if (strcmp(name,"lastIndexOf")==0){ if(!nargs) return NUM(-1); const char*sub=val_to_str(args[0]); int sl=(int)strlen(sub); for(int i=len-sl;i>=0;i--){ if(memcmp(s+i,sub,sl)==0) return NUM(i);} return NUM(-1); }
    if (strcmp(name,"includes")==0){ if(!nargs) return BOOLV(0); const char*sub=val_to_str(args[0]); int sl=(int)strlen(sub); for(int i=0;i+sl<=len;i++) if(memcmp(s+i,sub,sl)==0) return BOOLV(1); return BOOLV(0); }
    if (strcmp(name,"startsWith")==0){ if(!nargs) return BOOLV(0); const char*sub=val_to_str(args[0]); int sl=(int)strlen(sub); return BOOLV(sl<=len && memcmp(s,sub,sl)==0); }
    if (strcmp(name,"endsWith")==0){ if(!nargs) return BOOLV(0); const char*sub=val_to_str(args[0]); int sl=(int)strlen(sub); return BOOLV(sl<=len && memcmp(s+len-sl,sub,sl)==0); }
    if (strcmp(name,"trim")==0){ int a=0,b=len; while(a<b&&(s[a]==' '||s[a]=='\t'||s[a]=='\n'||s[a]=='\r'))a++; while(b>a&&(s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\n'||s[b-1]=='\r'))b--; char*r=aalloc(b-a+1); if(!r) return STRV(""); memcpy(r,s+a,b-a); r[b-a]=0; return STRV(r); }
    if (strcmp(name,"trimStart")==0){ int a=0; while(a<len&&(s[a]==' '||s[a]=='\t'||s[a]=='\n'||s[a]=='\r'))a++; char*r=aalloc(len-a+1); if(!r) return STRV(""); memcpy(r,s+a,len-a); r[len-a]=0; return STRV(r); }
    if (strcmp(name,"trimEnd")==0){ int b=len; while(b>0&&(s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\n'||s[b-1]=='\r'))b--; char*r=aalloc(b+1); if(!r) return STRV(""); memcpy(r,s,b); r[b]=0; return STRV(r); }
    if (strcmp(name,"repeat")==0){ int cnt=nargs?(int)to_num(args[0]):0; if(cnt<0)cnt=0; long total=(long)len*cnt; if(total>JS_ARENA){ rt_err("repeat too large"); return STRV(""); } char*r=aalloc(total+1); if(!r) return STRV(""); int p=0; for(int k=0;k<cnt;k++) for(int j=0;j<len;j++) r[p++]=s[j]; r[p]=0; return STRV(r); }
    if (strcmp(name,"search")==0){ regex *re=nargs?rx_of(args[0]):0; if(!re&&nargs) re=re_compile(val_to_str(args[0]),""); if(!re) return NUM(-1); int caps[2*(RE_MAXGROUP+1)]; return NUM(re_search(re,s,len,0,caps)); }
    if (strcmp(name,"match")==0){ regex *re=nargs?rx_of(args[0]):0; if(!re&&nargs) re=re_compile(val_to_str(args[0]),""); if(!re||!re->ok){ val nv=UND(); nv.t=V_NULL; return nv; }
        int caps[2*(RE_MAXGROUP+1)];
        if(re->global){ obj*a=new_obj(V_ARR); if(!a) return UND(); int pos=0,any=0; while(pos<=len){ int st=re_search(re,s,len,pos,caps); if(st<0) break; any=1; char*m=aalloc(caps[1]-caps[0]+1); if(m){memcpy(m,s+caps[0],caps[1]-caps[0]);m[caps[1]-caps[0]]=0;} arr_push_val(a,STRV(m?m:"")); pos = caps[1]>caps[0]?caps[1]:caps[1]+1; if(g_oom)break; } if(!any){ val nv=UND(); nv.t=V_NULL; return nv; } val r=UND();r.t=V_ARR;r.o=a;return r; }
        int st=re_search(re,s,len,0,caps); if(st<0){ val nv=UND(); nv.t=V_NULL; return nv; } return re_result(re,s,caps); }
    if (strcmp(name,"replace")==0 && nargs>=1 && rx_of(args[0])){ regex *re=rx_of(args[0]); const char *repl=nargs>1?val_to_str(args[1]):""; int caps[2*(RE_MAXGROUP+1)];
        sbuild b; memset(&b,0,sizeof(b)); int pos=0;
        for(;;){ int st=re_search(re,s,len,pos,caps); if(st<0||g_oom) break;
            sb_put(&b, s+pos, caps[0]-pos); sb_expand(&b, repl, s, caps, re->ngroup);
            pos = caps[1]>caps[0]?caps[1]:caps[1]+1; if(caps[1]==caps[0] && caps[0]<len) sb_put(&b, s+caps[0], 1);   /* zero-width: emit a char, advance */
            if(!re->global){ break; } }
        sb_put(&b, s+pos, len-pos); if(b.buf) b.buf[b.len]=0; return STRV(b.buf?b.buf:""); }
    if (strcmp(name,"replace")==0){ if(nargs<2) return STRV(s); const char*from=val_to_str(args[0]),*to=val_to_str(args[1]); int fl=(int)strlen(from),tl=(int)strlen(to); if(fl==0) return STRV(s); for(int i=0;i+fl<=len;i++){ if(memcmp(s+i,from,fl)==0){ char*r=aalloc((long)len-fl+tl+1); if(!r) return STRV(""); memcpy(r,s,i); memcpy(r+i,to,tl); memcpy(r+i+tl,s+i+fl,len-i-fl); r[len-fl+tl]=0; return STRV(r); } } return STRV(s); }
    if (strcmp(name,"replaceAll")==0){ if(nargs<2) return STRV(s); const char*from=val_to_str(args[0]),*to=val_to_str(args[1]); int fl=(int)strlen(from),tl=(int)strlen(to); if(fl==0) return STRV(s);
        int cnt=0; for(int i=0;i+fl<=len;){ if(memcmp(s+i,from,fl)==0){cnt++;i+=fl;} else i++; }
        long outlen=(long)len + (long)cnt*((long)tl-fl); if(outlen<0||outlen>JS_ARENA) return STRV(s);
        char*r=aalloc(outlen+1); if(!r) return STRV(""); int p=0;
        for(int i=0;i<len;){ if(i+fl<=len && memcmp(s+i,from,fl)==0){ memcpy(r+p,to,tl); p+=tl; i+=fl; } else r[p++]=s[i++]; }
        r[p]=0; return STRV(r); }
    if (strcmp(name,"at")==0){ int i=nargs?(int)to_num(args[0]):0; if(i<0) i+=len; if(i<0||i>=len) return UND(); char*r=aalloc(2); if(r){r[0]=s[i];r[1]=0;} return STRV(r?r:""); }
    if (strcmp(name,"padStart")==0||strcmp(name,"padEnd")==0){ int tgt=nargs?(int)to_num(args[0]):0; const char*pad=nargs>1?val_to_str(args[1]):" "; int pl=(int)strlen(pad); if(tgt<=len||pl==0||tgt>JS_ARENA){ char*r=aalloc(len+1); if(r){memcpy(r,s,len);r[len]=0;} return STRV(r?r:""); }
        char*r=aalloc(tgt+1); if(!r) return STRV(""); int start_at=name[3]=='S'?0:0; (void)start_at; int padn=tgt-len; int p=0;
        if(name[3]=='S'){ for(int i=0;i<padn;i++) r[p++]=pad[i%pl]; for(int i=0;i<len;i++) r[p++]=s[i]; }   /* padStart */
        else { for(int i=0;i<len;i++) r[p++]=s[i]; for(int i=0;i<padn;i++) r[p++]=pad[i%pl]; }              /* padEnd */
        r[p]=0; return STRV(r); }
    if (strcmp(name,"split")==0 && nargs>=1 && rx_of(args[0])){ regex *re=rx_of(args[0]); obj*arr=new_obj(V_ARR); if(!arr) return UND(); int caps[2*(RE_MAXGROUP+1)]; int start=0,pos=0;
        while(pos<=len){ int st=re_search(re,s,len,pos,caps); if(st<0||g_oom) break; if(caps[1]==caps[0]){ pos++; continue; }   /* skip zero-width to make progress */
            char*p=aalloc(caps[0]-start+1); if(p){memcpy(p,s+start,caps[0]-start);p[caps[0]-start]=0;} arr_push_val(arr,STRV(p?p:"")); start=caps[1]; pos=caps[1]; }
        char*p=aalloc(len-start+1); if(p){memcpy(p,s+start,len-start);p[len-start]=0;} arr_push_val(arr,STRV(p?p:"")); val v=UND();v.t=V_ARR;v.o=arr;return v; }
    if (strcmp(name,"split")==0){ obj*arr=new_obj(V_ARR); if(!arr) return UND(); const char*sep=nargs?val_to_str(args[0]):0; int sl=sep?(int)strlen(sep):-1;
        if(sl<0){ arr_push_val(arr,STRV(s)); }                       /* no separator: whole string */
        else if(sl==0){ for(int i=0;i<len;i++){ char*c=aalloc(2); if(c){c[0]=s[i];c[1]=0;} arr_push_val(arr,STRV(c?c:"")); } }  /* "" -> chars */
        else { int start=0; for(int i=0;i+sl<=len;){ if(memcmp(s+i,sep,sl)==0){ char*p=aalloc(i-start+1); if(p){memcpy(p,s+start,i-start);p[i-start]=0;} arr_push_val(arr,STRV(p?p:"")); i+=sl; start=i; } else i++; } char*p=aalloc(len-start+1); if(p){memcpy(p,s+start,len-start);p[len-start]=0;} arr_push_val(arr,STRV(p?p:"")); }
        val v=UND(); v.t=V_ARR; v.o=arr; return v; }
    rt_err("unknown string method"); return UND();
}
/* recursively flatten `src` into `r` up to `depth` levels (depth is caller-capped) */
static void flat_into(obj *r, obj *src, int depth){
    for(int i=0;i<src->n && !g_oom;i++){ val e=src->vals[i];
        if(depth>0 && e.t==V_ARR && e.o) flat_into(r, e.o, depth-1); else arr_push_val(r, e); }
}
static val eval_array_method(val recv, const char *name, val *args, int nargs) {
    obj *o=recv.o; if(!o) return UND();   /* a method that OOM'd can yield a NULL-backed array */
    if (strcmp(name,"push")==0){ for(int i=0;i<nargs;i++){ if(o->n>=o->cap){int nc=o->cap*2+4;val*nv=aalloc(sizeof(val)*nc);if(!nv){g_oom=1;break;}memcpy(nv,o->vals,sizeof(val)*o->n);o->vals=nv;o->cap=nc;} o->vals[o->n++]=args[i]; } return NUM(o->n); }
    if (strcmp(name,"pop")==0){ if(o->n==0) return UND(); return o->vals[--o->n]; }
    if (strcmp(name,"join")==0){
        const char*sep=nargs?val_to_str(args[0]):","; long sl=(long)strlen(sep);
        const char **parts=aalloc((long)sizeof(char*)*(o->n>0?o->n:1)); if(!parts) return STRV("");
        long total=0; for(int i=0;i<o->n;i++){ parts[i]=val_to_str(o->vals[i]); total+=(long)strlen(parts[i]); if(i) total+=sl; }
        char*buf=aalloc(total+1); if(!buf) return STRV(""); int p=0;
        for(int i=0;i<o->n;i++){ if(i){ for(long k=0;k<sl;k++) buf[p++]=sep[k]; } const char*v=parts[i]; while(*v) buf[p++]=*v++; }
        buf[p]=0; return STRV(buf);
    }
    if (strcmp(name,"indexOf")==0){ for(int i=0;i<o->n;i++){ val x=o->vals[i]; if(nargs&&x.t==args[0].t){ if(x.t==V_NUM&&x.num==args[0].num) return NUM(i); if(x.t==V_STR&&strcmp(x.str,args[0].str)==0) return NUM(i);} } return NUM(-1); }
    if (strcmp(name,"includes")==0){ for(int i=0;i<o->n;i++){ val x=o->vals[i]; if(nargs&&x.t==args[0].t){ if((x.t==V_NUM||x.t==V_BOOL)&&x.num==args[0].num) return BOOLV(1); if(x.t==V_STR&&strcmp(x.str,args[0].str)==0) return BOOLV(1);} } return BOOLV(0); }
    if (strcmp(name,"concat")==0){ obj*r=new_obj(V_ARR); if(!r) return UND(); for(int i=0;i<o->n;i++) arr_push_val(r,o->vals[i]); for(int a=0;a<nargs;a++){ if(args[a].t==V_ARR&&args[a].o){ for(int i=0;i<args[a].o->n;i++) arr_push_val(r,args[a].o->vals[i]); } else arr_push_val(r,args[a]); } val v=UND(); v.t=V_ARR; v.o=r; return v; }
    if (strcmp(name,"slice")==0){ int a=nargs>0?(int)to_num(args[0]):0, b=nargs>1?(int)to_num(args[1]):o->n; if(a<0)a+=o->n; if(b<0)b+=o->n; if(a<0)a=0; if(b>o->n)b=o->n; obj*r=new_obj(V_ARR); if(!r) return UND(); for(int i=a;i<b;i++) arr_push_val(r,o->vals[i]); val v=UND(); v.t=V_ARR; v.o=r; return v; }
    if (strcmp(name,"reverse")==0){ for(int i=0,j=o->n-1;i<j;i++,j--){ val t=o->vals[i]; o->vals[i]=o->vals[j]; o->vals[j]=t; } return recv; }
    if (strcmp(name,"fill")==0){ val fv=nargs?args[0]:UND(); int st=nargs>1?(int)to_num(args[1]):0, en=nargs>2?(int)to_num(args[2]):o->n; if(st<0)st+=o->n; if(en<0)en+=o->n; if(st<0)st=0; if(en>o->n)en=o->n; for(int i=st;i<en;i++) o->vals[i]=fv; return recv; }
    if (strcmp(name,"lastIndexOf")==0){ for(int i=o->n-1;i>=0;i--){ val x=o->vals[i]; if(nargs&&x.t==args[0].t){ if((x.t==V_NUM||x.t==V_BOOL)&&x.num==args[0].num) return NUM(i); if(x.t==V_STR&&strcmp(x.str,args[0].str)==0) return NUM(i);} } return NUM(-1); }
    if (strcmp(name,"flat")==0){ int depth=nargs?(int)to_num(args[0]):1; if(depth<0)depth=0; if(depth>64)depth=64; obj*r=new_obj(V_ARR); if(!r) return UND(); flat_into(r,o,depth); val v=UND(); v.t=V_ARR; v.o=r; return v; }
    if (strcmp(name,"forEach")==0){ if(nargs) for(int i=0;i<o->n && !g_err && !g_oom;i++){ val ca[2]={o->vals[i],NUM(i)}; call_function(args[0],ca,2); } return UND(); }
    if (strcmp(name,"map")==0){ obj*r=new_obj(V_ARR); if(!r) return UND(); if(nargs) for(int i=0;i<o->n && !g_err && !g_oom;i++){ val ca[2]={o->vals[i],NUM(i)}; arr_push_val(r,call_function(args[0],ca,2)); } val v=UND(); v.t=V_ARR; v.o=r; return v; }
    if (strcmp(name,"filter")==0){ obj*r=new_obj(V_ARR); if(!r) return UND(); if(nargs) for(int i=0;i<o->n && !g_err && !g_oom;i++){ val ca[2]={o->vals[i],NUM(i)}; if(truthy(call_function(args[0],ca,2))) arr_push_val(r,o->vals[i]); } val v=UND(); v.t=V_ARR; v.o=r; return v; }
    if (strcmp(name,"find")==0){ if(nargs) for(int i=0;i<o->n && !g_err && !g_oom;i++){ val ca[2]={o->vals[i],NUM(i)}; if(truthy(call_function(args[0],ca,2))) return o->vals[i]; } return UND(); }
    if (strcmp(name,"findLast")==0){ if(nargs) for(int i=o->n-1;i>=0 && !g_err && !g_oom;i--){ val ca[2]={o->vals[i],NUM(i)}; if(truthy(call_function(args[0],ca,2))) return o->vals[i]; } return UND(); }
    if (strcmp(name,"findLastIndex")==0){ if(nargs) for(int i=o->n-1;i>=0 && !g_err && !g_oom;i--){ val ca[2]={o->vals[i],NUM(i)}; if(truthy(call_function(args[0],ca,2))) return NUM(i); } return NUM(-1); }
    if (strcmp(name,"at")==0){ int i=nargs?(int)to_num(args[0]):0; if(i<0) i+=o->n; if(i>=0&&i<o->n) return o->vals[i]; return UND(); }
    if (strcmp(name,"flatMap")==0){ obj*r=new_obj(V_ARR); if(!r) return UND(); if(nargs) for(int i=0;i<o->n && !g_err && !g_oom;i++){ val ca[2]={o->vals[i],NUM(i)}; val m=call_function(args[0],ca,2); if(m.t==V_ARR&&m.o){ for(int j=0;j<m.o->n && !g_oom;j++) arr_push_val(r,m.o->vals[j]); } else arr_push_val(r,m); } val v=UND(); v.t=V_ARR; v.o=r; return v; }
    if (strcmp(name,"findIndex")==0){ if(nargs) for(int i=0;i<o->n && !g_err && !g_oom;i++){ val ca[2]={o->vals[i],NUM(i)}; if(truthy(call_function(args[0],ca,2))) return NUM(i); } return NUM(-1); }
    if (strcmp(name,"some")==0){ if(nargs) for(int i=0;i<o->n && !g_err && !g_oom;i++){ val ca[2]={o->vals[i],NUM(i)}; if(truthy(call_function(args[0],ca,2))) return BOOLV(1); } return BOOLV(0); }
    if (strcmp(name,"every")==0){ if(nargs) for(int i=0;i<o->n && !g_err && !g_oom;i++){ val ca[2]={o->vals[i],NUM(i)}; if(!truthy(call_function(args[0],ca,2))) return BOOLV(0); } return BOOLV(1); }
    if (strcmp(name,"reduce")==0){ if(!nargs) return UND(); int i=0; val acc; if(nargs>1) acc=args[1]; else { if(o->n==0) return UND(); acc=o->vals[0]; i=1; }
        for(; i<o->n && !g_err && !g_oom; i++){ val ca[3]={acc,o->vals[i],NUM(i)}; acc=call_function(args[0],ca,3); } return acc; }
    if (strcmp(name,"sort")==0){   /* in-place insertion sort: comparator if given, else string order (JS default) */
        int havecmp = (nargs && (args[0].t==V_FUN || args[0].t==V_NATIVE));
        for (int i=1; i<o->n && !g_err && !g_oom; i++){ val key=o->vals[i]; int j=i-1;
            while (j>=0){ int cmp;
                if (havecmp){ val ca[2]={o->vals[j],key}; cmp=(int)to_num(call_function(args[0],ca,2)); }
                else cmp=strcmp(val_to_str(o->vals[j]), val_to_str(key));
                if (cmp>0){ o->vals[j+1]=o->vals[j]; j--; } else break; }
            o->vals[j+1]=key; }
        return recv; }
    rt_err("unknown array method"); return UND();
}

/* ---- Map & Set ----
 * Both are V_OBJ values whose obj->kind is V_MAP/V_SET. A Map stores entries
 * interleaved in obj->vals as [k0,v0,k1,v1,…] (so n is 2*size); a Set stores
 * [v0,v1,…]. Lookup is a linear scan with `===` equality (val_equal). */
static val nat_map(val *args, int nargs){
    obj *o=new_obj(V_MAP); if(!o){g_oom=1;return UND();} o->n=0; val mv=obj_val(o);
    if (nargs>0 && args[0].t==V_ARR && args[0].o) {          /* new Map([[k,v],…]) */
        obj *src=args[0].o; for(int i=0;i<src->n && !g_oom;i++){ val e=src->vals[i]; if(e.t==V_ARR && e.o && e.o->n>=1){ val kv[2]={e.o->vals[0], e.o->n>1?e.o->vals[1]:UND()}; eval_map_method(mv,"set",kv,2); } }
    } else if (nargs>0 && args[0].t==V_OBJ && args[0].o && args[0].o->kind==V_MAP) {   /* new Map(otherMap) */
        obj *src=args[0].o; for(int i=0;i+1<src->n && !g_oom;i+=2){ val kv[2]={src->vals[i],src->vals[i+1]}; eval_map_method(mv,"set",kv,2); }
    }
    return mv;
}
static val nat_set(val *args, int nargs){
    obj *o=new_obj(V_SET); if(!o){g_oom=1;return UND();} o->n=0; val sv=obj_val(o);
    if (nargs>0) { val src=args[0];                          /* new Set([…]) / new Set("…") / new Set(otherSet) */
        if (src.t==V_ARR && src.o) for(int i=0;i<src.o->n && !g_oom;i++) eval_set_method(sv,"add",&src.o->vals[i],1);
        else if (src.t==V_OBJ && src.o && src.o->kind==V_SET) for(int i=0;i<src.o->n && !g_oom;i++) eval_set_method(sv,"add",&src.o->vals[i],1);
        else if (src.t==V_STR) { const char*s=src.str; for(int i=0;s[i]&&!g_oom;i++){ char*c=aalloc(2); if(c){c[0]=s[i];c[1]=0;} val cv=STRV(c?c:""); eval_set_method(sv,"add",&cv,1); } }
    }
    return sv;
}

static val eval_map_method(val recv, const char *name, val *args, int nargs) {
    obj *o=recv.o; val k = nargs>0?args[0]:UND();
    if (strcmp(name,"set")==0){ val v=nargs>1?args[1]:UND();
        for(int i=0;i+1<o->n;i+=2) if(val_equal(o->vals[i],k)){ o->vals[i+1]=v; return recv; }
        arr_push_val(o,k); arr_push_val(o,v); return recv; }                  /* returns the map (chainable) */
    if (strcmp(name,"get")==0){ for(int i=0;i+1<o->n;i+=2) if(val_equal(o->vals[i],k)) return o->vals[i+1]; return UND(); }
    if (strcmp(name,"has")==0){ for(int i=0;i+1<o->n;i+=2) if(val_equal(o->vals[i],k)) return BOOLV(1); return BOOLV(0); }
    if (strcmp(name,"delete")==0){ for(int i=0;i+1<o->n;i+=2) if(val_equal(o->vals[i],k)){ for(int j=i;j+2<o->n;j++) o->vals[j]=o->vals[j+2]; o->n-=2; return BOOLV(1); } return BOOLV(0); }
    if (strcmp(name,"clear")==0){ o->n=0; return UND(); }
    if (strcmp(name,"forEach")==0){ if(nargs<1) return UND(); for(int i=0;i+1<o->n && !g_err && !g_oom;i+=2){ val cb[3]={o->vals[i+1],o->vals[i],recv}; call_function(args[0],cb,3); } return UND(); }
    if (strcmp(name,"keys")==0||strcmp(name,"values")==0){ obj *a=new_obj(V_ARR); if(!a){g_oom=1;return UND();} int off=(name[0]=='v')?1:0; for(int i=0;i+1<o->n && !g_oom;i+=2) arr_push_val(a,o->vals[i+off]); val r=UND(); r.t=V_ARR; r.o=a; return r; }
    if (strcmp(name,"entries")==0){ obj *a=new_obj(V_ARR); if(!a){g_oom=1;return UND();} for(int i=0;i+1<o->n && !g_oom;i+=2){ obj *pair=new_obj(V_ARR); if(!pair){g_oom=1;break;} arr_push_val(pair,o->vals[i]); arr_push_val(pair,o->vals[i+1]); val pv=UND();pv.t=V_ARR;pv.o=pair; arr_push_val(a,pv); } val r=UND(); r.t=V_ARR; r.o=a; return r; }
    rt_err("unknown Map method"); return UND();
}
static val eval_set_method(val recv, const char *name, val *args, int nargs) {
    obj *o=recv.o; val v = nargs>0?args[0]:UND();
    if (strcmp(name,"add")==0){ for(int i=0;i<o->n;i++) if(val_equal(o->vals[i],v)) return recv; arr_push_val(o,v); return recv; }
    if (strcmp(name,"has")==0){ for(int i=0;i<o->n;i++) if(val_equal(o->vals[i],v)) return BOOLV(1); return BOOLV(0); }
    if (strcmp(name,"delete")==0){ for(int i=0;i<o->n;i++) if(val_equal(o->vals[i],v)){ for(int j=i;j+1<o->n;j++) o->vals[j]=o->vals[j+1]; o->n--; return BOOLV(1); } return BOOLV(0); }
    if (strcmp(name,"clear")==0){ o->n=0; return UND(); }
    if (strcmp(name,"forEach")==0){ if(nargs<1) return UND(); for(int i=0;i<o->n && !g_err && !g_oom;i++){ val cb[3]={o->vals[i],o->vals[i],recv}; call_function(args[0],cb,3); } return UND(); }
    if (strcmp(name,"values")==0||strcmp(name,"keys")==0){ obj *a=new_obj(V_ARR); if(!a){g_oom=1;return UND();} for(int i=0;i<o->n && !g_oom;i++) arr_push_val(a,o->vals[i]); val r=UND(); r.t=V_ARR; r.o=a; return r; }
    rt_err("unknown Set method"); return UND();
}

/* native print/console.log */
static val native_print(val *args, int nargs) {
    for (int i=0;i<nargs;i++){ if(i) out_str(" "); out_str(val_to_str(args[i])); }
    out_str("\n"); return UND();
}

/* document.write(...): when a host (the browser) registers g_doc_write, the joined
 * argument string is handed to it (to splice into the page); otherwise it falls
 * back to the normal output stream so `js` at the shell still shows it. */
static void (*g_doc_write)(const char *s);
static val native_doc_write(val *args, int nargs) {
    for (int i=0;i<nargs;i++){ const char *s=val_to_str(args[i]); if(g_doc_write) g_doc_write(s); else out_str(s); }
    return UND();
}

/* localStorage: a host-provided key->value (string) store that survives the
 * per-run arena reset, so page click-handlers can accumulate state (a counter). */
static const char *(*g_ls_get)(const char *key);
static void        (*g_ls_set)(const char *key, const char *val);
static val native_ls_getItem(val *args, int nargs) {
    if (!g_ls_get || !nargs) { val v=UND(); v.t=V_NULL; return v; }
    const char *r = g_ls_get(val_to_str(args[0]));
    if (!r) { val v=UND(); v.t=V_NULL; return v; }
    return STRV(intern(r, (int)strlen(r)));               /* copy out (host buffer may change) */
}
static val native_ls_setItem(val *args, int nargs) {
    if (g_ls_set && nargs>=2) g_ls_set(val_to_str(args[0]), val_to_str(args[1]));
    return UND();
}

/* Date() -> current wall-clock as "YYYY-MM-DD HH:MM:SS" (a useful subset of the
 * real Date). Reads the CMOS RTC directly (kernel); a fixed string on the host. */
static void d2(char *b, int *p, int v){ b[(*p)++]='0'+(v/10)%10; b[(*p)++]='0'+v%10; }
static val nat_date(val *a, int n){
    (void)a; (void)n; char buf[24]; int p=0;
#ifndef JS_HOSTTEST
    struct rtc_time t; rtc_now(&t);
    int y=t.year; buf[p++]='0'+(y/1000)%10; buf[p++]='0'+(y/100)%10; buf[p++]='0'+(y/10)%10; buf[p++]='0'+y%10;
    buf[p++]='-'; d2(buf,&p,t.month); buf[p++]='-'; d2(buf,&p,t.day);
    buf[p++]=' '; d2(buf,&p,t.hour); buf[p++]=':'; d2(buf,&p,t.min); buf[p++]=':'; d2(buf,&p,t.sec);
    buf[p]=0;
#else
    const char *s="2026-06-13 12:00:00"; while(s[p]){buf[p]=s[p];p++;} buf[p]=0;
#endif
    return STRV(intern(buf, p));
}

/* ---- Math (integer; the kernel has no FPU) ---- */
static int64_t iabs64(int64_t x){ return x < 0 ? -x : x; }
static val nat_abs(val *a, int n){ return NUM(n ? iabs64(to_num(a[0])) : 0); }
static val nat_max(val *a, int n){ if(!n) return UND(); int64_t m=to_num(a[0]); for(int i=1;i<n;i++){int64_t v=to_num(a[i]); if(v>m)m=v;} return NUM(m); }
static val nat_min(val *a, int n){ if(!n) return UND(); int64_t m=to_num(a[0]); for(int i=1;i<n;i++){int64_t v=to_num(a[i]); if(v<m)m=v;} return NUM(m); }
static val nat_ident(val *a, int n){ return NUM(n ? to_num(a[0]) : 0); }   /* floor/ceil/round are identity on ints */
static val nat_sqrt(val *a, int n){ int64_t x=n?to_num(a[0]):0; if(x<1) return NUM(0); int64_t lo=0, hi=(x<2?x:x/2+1); if(hi>3037000499LL) hi=3037000499LL; while(lo<hi){ int64_t mid=lo+(hi-lo+1)/2; if(mid<=x/mid) lo=mid; else hi=mid-1; } return NUM(lo); }
static val nat_pow(val *a, int n){ int64_t b=n>0?to_num(a[0]):0, e=n>1?to_num(a[1]):0; int64_t r=1; for(int64_t i=0;i<e && i<63;i++) r*=b; return NUM(r); }

/* ---- global functions ---- */
static val nat_parseInt(val *a, int n){                                            /* parseInt(str, radix), with 0x detection */
    if(!n) return NUM(0);
    const char *s=val_to_str(a[0]); int radix=(n>1)?(int)to_num(a[1]):0;
    while(*s==' '||*s=='\t'||*s=='\n'||*s=='\r') s++;
    int neg=0; if(*s=='+') s++; else if(*s=='-'){ neg=1; s++; }
    if((radix==0||radix==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')){ radix=16; s+=2; }
    if(radix<2||radix>36) radix=10;
    int64_t v=0; int any=0;
    for(; *s; s++){ int d; char c=*s;
        if(c>='0'&&c<='9') d=c-'0'; else if(c>='a'&&c<='z') d=c-'a'+10; else if(c>='A'&&c<='Z') d=c-'A'+10; else break;
        if(d>=radix) break; v=v*radix+d; any=1; }
    return NUM(any?(neg?-v:v):0);
}
static val nat_String(val *a, int n){ return STRV(n ? val_to_str(a[0]) : ""); }
static val nat_Number(val *a, int n){ return NUM(n ? to_num(a[0]) : 0); }
static val nat_Boolean(val *a, int n){ return BOOLV(n ? truthy(a[0]) : 0); }
static val nat_isNaN(val *a, int n){ (void)a; (void)n; return BOOLV(0); }          /* integer Number is never NaN */

/* ---- Object.keys(o) -> array of key strings ---- */
static val nat_obj_keys(val *a, int n){
    obj *r = new_obj(V_ARR); if(!r) return UND();
    if (n && (a[0].t==V_OBJ)) {
        obj *o=a[0].o;
        for (int i=0;i<o->n;i++){ if(r->n>=r->cap){int nc=r->cap*2+2;val*nv=aalloc((long)sizeof(val)*nc); if(!nv){g_oom=1;break;} memcpy(nv,r->vals,sizeof(val)*r->n); r->vals=nv; r->cap=nc;} r->vals[r->n++]=STRV(o->keys[i]); }
    }
    val v=UND(); v.t=V_ARR; v.o=r; return v;
}

/* ---- JSON.stringify (bounded 16 KB output; depth-guarded against cycles) ---- */
static char *g_json; static int g_json_pos, g_json_cap;
static void js_app(const char *s){ while(*s && g_json_pos<g_json_cap-1) g_json[g_json_pos++]=*s++; }
static void js_appq(const char *s){ js_app("\""); for(; *s && g_json_pos<g_json_cap-2; s++){ char c=*s;
        if(c=='"'||c=='\\'){ g_json[g_json_pos++]='\\'; g_json[g_json_pos++]=c; }
        else if(c=='\n'){ js_app("\\n"); } else if(c=='\t'){ js_app("\\t"); } else if(c=='\r'){ js_app("\\r"); }
        else g_json[g_json_pos++]=c; } js_app("\""); }
static int g_json_pretty; static char g_json_unit[16];   /* indentation: "" (compact) or N spaces / a string */
static void js_nl(int depth){ if(!g_json_pretty) return; js_app("\n"); for(int i=0;i<depth && i<64;i++) js_app(g_json_unit); }
static void json_val(val v, int depth){
    if(++g_depth>MAXDEPTH){ g_depth--; js_app("null"); return; }
    switch(v.t){
        case V_BOOL: js_app(v.num?"true":"false"); break;
        case V_NUM:  js_app(i64_to_str(v.num)); break;
        case V_STR:  js_appq(v.str); break;
        case V_ARR:  if(v.o->n==0){ js_app("[]"); break; } js_app("[");
            for(int i=0;i<v.o->n;i++){ if(i) js_app(","); js_nl(depth+1); json_val(v.o->vals[i], depth+1); } js_nl(depth); js_app("]"); break;
        case V_OBJ:  if(v.o->n==0){ js_app("{}"); break; } js_app("{");
            for(int i=0;i<v.o->n;i++){ if(i) js_app(","); js_nl(depth+1); js_appq(v.o->keys[i]); js_app(g_json_pretty?": ":":"); json_val(v.o->vals[i], depth+1); } js_nl(depth); js_app("}"); break;
        default:     js_app("null"); break;   /* undefined/null/function */
    }
    g_depth--;
}
static val nat_json_stringify(val *a, int n){ if(!n) return UND(); char *buf=aalloc(16384); if(!buf) return STRV("");
    g_json=buf; g_json_pos=0; g_json_cap=16384; g_json_pretty=0; g_json_unit[0]=0;
    if(n>2){ val sp=a[2];                         /* a[1] is the (ignored) replacer; a[2] is the indent */
        if(sp.t==V_NUM){ int k=(int)sp.num; if(k<0)k=0; if(k>10)k=10; for(int i=0;i<k;i++) g_json_unit[i]=' '; g_json_unit[k]=0; if(k>0) g_json_pretty=1; }
        else if(sp.t==V_STR){ int i=0; for(; sp.str[i] && i<15; i++) g_json_unit[i]=sp.str[i]; g_json_unit[i]=0; if(i>0) g_json_pretty=1; }
    }
    json_val(a[0], 0); buf[g_json_pos]=0; return STRV(buf); }

/* ---- JSON.parse (recursive descent; bounded + depth-guarded) ---- */
static const char *jp, *jp_end; static int jp_err;
static void jp_ws(void){ while(jp<jp_end && (*jp==' '||*jp=='\t'||*jp=='\n'||*jp=='\r')) jp++; }
static val json_parse_val(void);
static const char *jp_string(void){          /* assumes *jp == '"'; sizes from the raw token */
    jp++; const char *raw=jp, *e=jp;
    while(e<jp_end && *e!='"'){ if(*e=='\\' && e+1<jp_end) e++; e++; }
    int cap=(int)(e-raw)+1; char *buf=aalloc(cap); int nn=0;
    while(jp<e){ char c=*jp++; if(c=='\\' && jp<jp_end){ char x=*jp++; c = x=='n'?'\n':x=='t'?'\t':x=='r'?'\r':x=='"'?'"':x=='\\'?'\\':x=='/'?'/':x; if(x=='u'){ for(int k=0;k<4 && jp<jp_end;k++) jp++; c='?'; } } if(buf && nn<cap-1) buf[nn++]=c; }
    if(jp<jp_end && *jp=='"') jp++;
    if(buf) buf[nn]=0; return buf?buf:"";
}
static val json_parse_val(void){
    if(++g_depth>MAXDEPTH){ g_depth--; jp_err=1; return UND(); }
    val r=UND(); jp_ws();
    if(jp>=jp_end) jp_err=1;
    else if(*jp=='{'){ obj*o=new_obj(V_OBJ); if(!o){ jp_err=1; g_depth--; return UND(); } jp++; jp_ws();
        if(jp<jp_end && *jp=='}') jp++;
        else for(;;){ jp_ws(); if(jp>=jp_end||*jp!='"'){ jp_err=1; break; } const char*k=jp_string(); jp_ws();
            if(jp<jp_end && *jp==':') jp++; else { jp_err=1; break; }
            val v=json_parse_val(); if(o) obj_set(o,k,v); jp_ws();
            if(jp<jp_end && *jp==','){ jp++; continue; } if(jp<jp_end && *jp=='}'){ jp++; } else jp_err=1; break; }
        r=obj_val(o); }
    else if(*jp=='['){ obj*o=new_obj(V_ARR); if(!o){ jp_err=1; g_depth--; return UND(); } jp++; jp_ws();
        if(jp<jp_end && *jp==']') jp++;
        else for(;;){ val v=json_parse_val(); if(o) arr_push_val(o,v); jp_ws();
            if(jp<jp_end && *jp==','){ jp++; continue; } if(jp<jp_end && *jp==']'){ jp++; } else jp_err=1; break; }
        val vv=UND(); vv.t=V_ARR; vv.o=o; r=vv; }
    else if(*jp=='"'){ r=STRV(jp_string()); }
    else if(*jp=='t'){ r=BOOLV(1); while(jp<jp_end && *jp>='a' && *jp<='z') jp++; }
    else if(*jp=='f'){ r=BOOLV(0); while(jp<jp_end && *jp>='a' && *jp<='z') jp++; }
    else if(*jp=='n'){ r.t=V_NULL; while(jp<jp_end && *jp>='a' && *jp<='z') jp++; }
    else if(*jp=='-' || (*jp>='0'&&*jp<='9')){ int neg=0; if(*jp=='-'){ neg=1; jp++; } int64_t v=0;
        while(jp<jp_end && *jp>='0'&&*jp<='9'){ v=(int64_t)((uint64_t)v*10+(unsigned)(*jp-'0')); jp++; }
        if(jp<jp_end && *jp=='.'){ jp++; while(jp<jp_end && *jp>='0'&&*jp<='9') jp++; }   /* integer Number: drop fraction */
        r=NUM(neg?-v:v); }
    else jp_err=1;
    g_depth--; return r;
}
static val nat_json_parse(val *a, int n){ if(!n || a[0].t!=V_STR) return UND(); const char *s=a[0].str; jp=s; jp_end=s+strlen(s); jp_err=0; val r=json_parse_val(); if(jp_err){ rt_err("JSON.parse: invalid JSON"); return UND(); } return r; }

/* ---- Object.values / Object.entries, Array.isArray / Array.from ---- */
static val nat_obj_values(val *a, int n){
    obj *r=new_obj(V_ARR); if(!r) return UND();
    if (n && a[0].t==V_OBJ && a[0].o) for (int i=0;i<a[0].o->n;i++) arr_push_val(r, a[0].o->vals[i]);
    val v=UND(); v.t=V_ARR; v.o=r; return v;
}
static val nat_obj_entries(val *a, int n){
    obj *r=new_obj(V_ARR); if(!r) return UND();
    if (n && a[0].t==V_OBJ && a[0].o) for (int i=0;i<a[0].o->n;i++){
        obj *pair=new_obj(V_ARR); if(!pair) break; arr_push_val(pair, STRV(a[0].o->keys[i])); arr_push_val(pair, a[0].o->vals[i]);
        val pv=UND(); pv.t=V_ARR; pv.o=pair; arr_push_val(r, pv); }
    val v=UND(); v.t=V_ARR; v.o=r; return v;
}
static val nat_array_isArray(val *a, int n){ return BOOLV(n && a[0].t==V_ARR); }
/* Object.fromEntries([[k,v],…]) or fromEntries(map) -> { k: v, … } */
static val nat_obj_fromEntries(val *a, int n){
    obj *r=new_obj(V_OBJ); if(!r) return UND();
    if (n && a[0].t==V_OBJ && a[0].o && a[0].o->kind==V_MAP) {           /* a Map: entries are interleaved [k,v,…] */
        obj *m=a[0].o; for (int i=0;i+1<m->n && !g_oom;i+=2) obj_set(r, val_to_str(m->vals[i]), m->vals[i+1]);
    } else if (n && a[0].t==V_ARR && a[0].o) {                           /* an array of [k,v] pairs */
        obj *arr=a[0].o; for (int i=0;i<arr->n && !g_oom;i++){ val e=arr->vals[i];
            if (e.t==V_ARR && e.o && e.o->n>=1) obj_set(r, val_to_str(e.o->vals[0]), e.o->n>1?e.o->vals[1]:UND()); }
    }
    return obj_val(r);
}
static val nat_obj_assign(val *a, int n){   /* Object.assign(target, ...sources) -> target */
    if (!n || a[0].t!=V_OBJ || !a[0].o) return n?a[0]:UND();
    for (int i=1;i<n;i++) if (a[i].t==V_OBJ && a[i].o) for (int j=0;j<a[i].o->n;j++) obj_set(a[0].o, a[i].o->keys[j], a[i].o->vals[j]);
    return a[0];
}
static int64_t nat_sign_v(int64_t x){ return x>0?1:x<0?-1:0; }
static val nat_sign(val *a, int n){ return NUM(n?nat_sign_v(to_num(a[0])):0); }
/* push `e` onto r, applying the optional Array.from map function (e, index) */
static void from_push(obj *r, val e, val fn, int hasfn){ if(hasfn){ val ca[2]={e,NUM(r->n)}; e=call_function(fn,ca,2); } arr_push_val(r,e); }
static val nat_array_from(val *a, int n){
    obj *r=new_obj(V_ARR); if(!r) return UND();
    val fn = n>1?a[1]:UND(); int hasfn=(fn.t==V_FUN||fn.t==V_NATIVE);
    if (n && a[0].t==V_ARR && a[0].o) for (int i=0;i<a[0].o->n && !g_oom;i++) from_push(r, a[0].o->vals[i], fn, hasfn);
    else if (n && a[0].t==V_OBJ && a[0].o && a[0].o->kind==V_SET) for (int i=0;i<a[0].o->n && !g_oom;i++) from_push(r, a[0].o->vals[i], fn, hasfn);   /* Array.from(set) — dedup idiom */
    else if (n && a[0].t==V_OBJ && a[0].o && a[0].o->kind==V_MAP) for (int i=0;i+1<a[0].o->n && !g_oom;i+=2){ obj*p=new_obj(V_ARR); if(!p){g_oom=1;break;} arr_push_val(p,a[0].o->vals[i]); arr_push_val(p,a[0].o->vals[i+1]); val pv=UND();pv.t=V_ARR;pv.o=p; from_push(r, pv, fn, hasfn); }
    else if (n && a[0].t==V_STR) { const char*s=a[0].str; for (int i=0;s[i] && !g_oom;i++){ char*c=aalloc(2); if(c){c[0]=s[i];c[1]=0;} from_push(r, STRV(c?c:""), fn, hasfn); } }
    val v=UND(); v.t=V_ARR; v.o=r; return v;
}
static val nat_array_of(val *a, int n){ obj *r=new_obj(V_ARR); if(!r) return UND(); for(int i=0;i<n && !g_oom;i++) arr_push_val(r,a[i]); val v=UND(); v.t=V_ARR; v.o=r; return v; }

/* register a native function on object `o` under `name` */
static void def_native(obj *o, const char *name, val (*fn)(val*,int)){
    obj *f=new_obj(V_NATIVE); if(!f) return; f->native=fn; val v=UND(); v.t=V_NATIVE; v.o=f; obj_set(o,name,v);
}
static val obj_val(obj *o){ val v=UND(); v.t=V_OBJ; v.o=o; return v; }
static val obj_val_native(obj *o){ val v=UND(); v.t=V_NATIVE; v.o=o; return v; }

static void install_globals(env *g) {
    obj *p=new_obj(V_NATIVE); p->native=native_print; val pv=UND(); pv.t=V_NATIVE; pv.o=p; env_define(g,"print",pv);
    /* console.log */
    obj *log=new_obj(V_NATIVE); log->native=native_print; val lv=UND(); lv.t=V_NATIVE; lv.o=log;
    obj *con=new_obj(V_OBJ); obj_set(con,"log",lv); val cv=UND(); cv.t=V_OBJ; cv.o=con; env_define(g,"console",cv);
    /* document: write() splices HTML into the page. getElementById() returns
     * undefined for now (a real DOM is future work) — scripts that touch the
     * result no-op safely rather than crash. */
    obj *dw=new_obj(V_NATIVE); dw->native=native_doc_write; val dwv=UND(); dwv.t=V_NATIVE; dwv.o=dw;
    obj *doc=new_obj(V_OBJ); obj_set(doc,"write",dwv); obj_set(doc,"writeln",dwv);
    val docv=UND(); docv.t=V_OBJ; docv.o=doc; env_define(g,"document",docv);
    /* localStorage.getItem/setItem (browser-backed; no-ops at the shell) */
    obj *ls=new_obj(V_OBJ); def_native(ls,"getItem",native_ls_getItem); def_native(ls,"setItem",native_ls_setItem);
    env_define(g,"localStorage",obj_val(ls));

    /* Math */
    obj *math=new_obj(V_OBJ);
    def_native(math,"abs",nat_abs); def_native(math,"max",nat_max); def_native(math,"min",nat_min);
    def_native(math,"floor",nat_ident); def_native(math,"ceil",nat_ident); def_native(math,"round",nat_ident); def_native(math,"trunc",nat_ident);
    def_native(math,"sqrt",nat_sqrt); def_native(math,"pow",nat_pow); def_native(math,"sign",nat_sign);
    env_define(g,"Math",obj_val(math));
    /* Object (Object.keys) */
    obj *objc=new_obj(V_OBJ); def_native(objc,"keys",nat_obj_keys); def_native(objc,"values",nat_obj_values); def_native(objc,"entries",nat_obj_entries); def_native(objc,"assign",nat_obj_assign); def_native(objc,"fromEntries",nat_obj_fromEntries); env_define(g,"Object",obj_val(objc));
    { obj *mp=new_obj(V_NATIVE); if(mp){ mp->native=nat_map; val v=UND(); v.t=V_NATIVE; v.o=mp; env_define(g,"Map",v); } }   /* new Map() */
    { obj *st=new_obj(V_NATIVE); if(st){ st->native=nat_set; val v=UND(); v.t=V_NATIVE; v.o=st; env_define(g,"Set",v); } }   /* new Set() */
    { obj *rx=new_obj(V_NATIVE); if(rx){ rx->native=nat_regexp; val v=UND(); v.t=V_NATIVE; v.o=rx; env_define(g,"RegExp",v); } }   /* RegExp(pat,flags) / new RegExp(...) */
    obj *arrc=new_obj(V_OBJ); def_native(arrc,"isArray",nat_array_isArray); def_native(arrc,"from",nat_array_from); def_native(arrc,"of",nat_array_of); env_define(g,"Array",obj_val(arrc));
    /* JSON (stringify) */
    obj *json=new_obj(V_OBJ); def_native(json,"stringify",nat_json_stringify); def_native(json,"parse",nat_json_parse); env_define(g,"JSON",obj_val(json));
    /* global functions */
    obj *pi=new_obj(V_NATIVE); pi->native=nat_parseInt; env_define(g,"parseInt",obj_val_native(pi));
    obj *pf=new_obj(V_NATIVE); pf->native=nat_parseInt; env_define(g,"parseFloat",obj_val_native(pf));
    obj *sf=new_obj(V_NATIVE); sf->native=nat_String;   env_define(g,"String",obj_val_native(sf));
    obj *nf=new_obj(V_NATIVE); nf->native=nat_Number;   env_define(g,"Number",obj_val_native(nf));
    obj *bf=new_obj(V_NATIVE); bf->native=nat_Boolean;  env_define(g,"Boolean",obj_val_native(bf));
    obj *nan=new_obj(V_NATIVE); nan->native=nat_isNaN;  env_define(g,"isNaN",obj_val_native(nan));
    obj *dt=new_obj(V_NATIVE); dt->native=nat_date;     env_define(g,"Date",obj_val_native(dt));   /* Date() -> wall-clock string */
}

/* =========================== entry point =========================== */
/* Run JS source; print output into out[0..outmax). Returns output length, or -1
 * on error (with the message appended to the output). Serialized: uses static
 * arena + globals, so only one js_run() may be in flight at a time. */
/* Interpreter state is static (arena + globals), so only one run may be in flight.
 * The browser (WM thread) and the shell's `js` (a ring-3 syscall) are distinct
 * preemptible tasks, so guard with an irq-protected flag like tls_get does. */
#ifndef JS_HOSTTEST
static inline unsigned long js_irq_save(void){ unsigned long f; __asm__ volatile("pushfq; pop %0; cli":"=r"(f)::"memory"); return f; }
static inline void js_irq_restore(unsigned long f){ __asm__ volatile("push %0; popfq"::"r"(f):"memory","cc"); }
#else
static inline unsigned long js_irq_save(void){ return 0; }
static inline void js_irq_restore(unsigned long f){ (void)f; }
#endif
static volatile int js_busy;

static int js_run_impl(const char *src, char *out, int outmax) {
    unsigned long f = js_irq_save();
    if (js_busy) { js_irq_restore(f); if (outmax) out[0]=0; return -1; }   /* another run in flight */
    js_busy = 1; js_irq_restore(f);

    g_arena_off=0; g_oom=0; g_err=0; g_errmsg[0]=0; g_depth=0;
    g_out=out; g_out_cap=outmax; g_out_len=0; if(outmax) out[0]=0;

    lexer L; memset(&L,0,sizeof(L)); L.src=src; L.len=(int)strlen(src); L.pos=0;
    node *prog = parse_program(&L);
    g_depth = 0;                          /* parse balances g_depth to 0; reset defensively for eval */
    if (!g_err && !g_oom) {
        env *g = new_env(0);
        if (g) { install_globals(g); eval_stmt(prog, g); }
    }
    if (g_oom) rt_err("out of memory (arena)");
    int r = g_out_len;
    if (g_err) { out_str("\n[js error: "); out_str(g_errmsg); out_str("]\n"); r = -1; }
    js_busy = 0;
    return r;
}

int js_run(const char *src, char *out, int outmax) {
    g_doc_write = 0;                      /* shell `js`: document.write falls back to output */
    g_ls_get = 0; g_ls_set = 0;           /* and no persistent storage */
    return js_run_impl(src, out, outmax);
}

/* The browser registers a localStorage backing store before running page JS. */
void js_set_storage(const char *(*get)(const char *), void (*set)(const char *, const char *)) {
    g_ls_get = get; g_ls_set = set;
}

/* Run page scripts with a host document.write sink (the browser splices the
 * written HTML into the page and re-parses). */
int js_run_doc(const char *src, char *out, int outmax, void (*write_cb)(const char *)) {
    g_doc_write = write_cb;
    int r = js_run_impl(src, out, outmax);
    g_doc_write = 0;
    return r;
}

#ifdef JS_HOSTTEST
/* a tiny in-memory localStorage so host tests can exercise the persistent path */
static char hk[16][32], hv[16][160]; static int hn;
static const char *host_get(const char *k){ for(int i=0;i<hn;i++) if(!strcmp(hk[i],k)) return hv[i]; return 0; }
static void host_set(const char *k, const char *v){
    int i; for(i=0;i<hn;i++) if(!strcmp(hk[i],k)) break;
    if(i==hn){ if(hn>=16) return; int j=0; while(k[j]&&j<31){hk[hn][j]=k[j];j++;} hk[hn][j]=0; i=hn++; }
    int j=0; while(v[j]&&j<159){hv[i][j]=v[j];j++;} hv[i][j]=0;
}
int main(int argc, char **argv) {
    static char src[200000]; int n=0; FILE *f = argc>1?fopen(argv[1],"rb"):stdin;
    n = (int)fread(src,1,sizeof(src)-1,f); src[n]=0;
    static char outb[200000];
    js_set_storage(host_get, host_set);                 /* mirror the browser: storage + js_run_doc */
    int r = js_run_doc(src, outb, sizeof(outb), 0);
    fputs(outb, stdout);
    return r<0?1:0;
}
#endif
