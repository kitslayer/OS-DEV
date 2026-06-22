/* dmath.h — double math for the calculator app, copied VERBATIM from kernel/js.c
 * (the JS engine's IEEE-754 helpers: lines ~976-1110) so it's already correct +
 * fuzz-tested. NO libm / no libcall: everything is from-scratch (sqrt = Newton,
 * trig = range-reduced Taylor, ln/exp = series, pow = exp(e*ln b)). Freestanding,
 * pure, no syscalls — calc.c #includes it and the calc object is built with SSE.
 *
 * Differences from js.c (the ONLY edits): num_to_str/i64_to_str there allocate via
 * the JS arena (aalloc), which does not exist in the calc app. Here they are renamed
 * (dnum_to_str / d_i64_to_str) and write into file-static char buffers instead. Each
 * has its OWN buffer, and dnum_to_str's integral path returns d_i64_to_str's buffer,
 * so a single dnum_to_str(...) result stays valid until the next dnum_to_str call
 * (calc.c prints it immediately, so that's safe). The math bodies are unchanged. */
#ifndef DMATH_H
#define DMATH_H

#include <stdint.h>

/* ---- IEEE-754 double helpers (built with SSE; verbatim from js.c) ---- */
#define JS_INF (__builtin_inf())
#define JS_NAN (__builtin_nan(""))
static int js_isnan(double x){ return x != x; }
static int js_isfinite(double x){ return (x - x) == 0.0; }   /* finite -> 0; inf/nan -> nan != 0 */
static int js_isinf(double x){ return js_isfinite(x) ? 0 : !js_isnan(x); }
static double js_trunc(double x){            /* toward zero */
    if (!js_isfinite(x)) return x;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;   /* >= 2^52: already integral */
    return (double)(int64_t)x;
}
static double js_floor(double x){ double t=js_trunc(x); return (js_isfinite(x) && t>x) ? t-1.0 : t; }
static double js_ceil (double x){ double t=js_trunc(x); return (js_isfinite(x) && t<x) ? t+1.0 : t; }
static double js_round(double x){ return js_isfinite(x) ? js_floor(x+0.5) : x; }   /* JS Math.round = floor(x+0.5) */
static double js_fabs (double x){ return x<0 ? -x : x; }
static double js_fmod (double a, double b){   /* JS %: sign of a; a - b*trunc(a/b); x%0 and Inf%y -> NaN */
    if (js_isnan(a) || js_isnan(b) || js_isinf(a) || b==0.0) return JS_NAN;
    if (js_isinf(b)) return a;
    return a - js_trunc(a/b)*b;
}
static double js_sqrt(double x){             /* Newton; no libm/libcall (freestanding kernel + host) */
    if (x < 0.0) return JS_NAN;
    if (x == 0.0 || !js_isfinite(x)) return x;   /* 0, +Inf, NaN pass through */
    double g = x > 1.0 ? x : 1.0;
    for (int i=0;i<64;i++){ double ng=0.5*(g + x/g); if (ng==g) break; g=ng; }
    return g;
}
static double js_ln(double x){               /* natural log; x>0 */
    if (x<=0.0) return x==0.0 ? -JS_INF : JS_NAN;
    int k=0; while (x>=2.0){ x*=0.5; k++; } while (x<1.0){ x*=2.0; k--; }   /* x = m*2^k, m in [1,2) */
    double t=(x-1.0)/(x+1.0), t2=t*t, term=t, sum=0.0;                      /* ln(m)=2*(t+t^3/3+t^5/5+..) */
    for (int i=1;i<=25;i+=2){ sum += term/i; term *= t2; }
    return 2.0*sum + (double)k*0.69314718055994530942;
}
static double js_exp(double x){
    if (x==0.0) return 1.0;
    if (js_isinf(x)) return x<0 ? 0.0 : JS_INF;
    double ln2=0.69314718055994530942; double kf=js_round(x/ln2); int k=(int)kf;   /* x = k*ln2 + r */
    double r=x-kf*ln2, term=1.0, sum=1.0;
    for (int i=1;i<=20;i++){ term *= r/i; sum += term; }
    double p=1.0; if (k>=0){ for(int i=0;i<k;i++) p*=2.0; } else { for(int i=0;i<-k;i++) p*=0.5; }
    return sum*p;
}
static double js_pow(double b, double e){
    if (e==0.0) return 1.0;
    if (e==js_trunc(e) && e>=-1024.0 && e<=1024.0){            /* integer exponent: exact repeated multiply */
        int neg=e<0; int64_t n=(int64_t)(neg?-e:e); double r=1.0;
        for (int64_t i=0;i<n;i++) r*=b;
        return neg ? 1.0/r : r;
    }
    if (b==0.0) return 0.0;
    if (b<0.0) return JS_NAN;                                  /* negative base, fractional exp -> NaN */
    return js_exp(e*js_ln(b));                                 /* general b^e = exp(e*ln b) */
}
/* ---- trig (range-reduced Taylor; ~12-15 digits over the reduced range) ---- */
static double js_sin(double x){
    if (!js_isfinite(x)) return JS_NAN;
    double twopi=6.283185307179586, pi=3.141592653589793;
    x = js_fmod(x, twopi); if (x>pi) x-=twopi; else if (x<-pi) x+=twopi;   /* reduce to [-pi, pi] */
    double term=x, sum=x, x2=x*x;
    for (int i=1;i<=12;i++){ term *= -x2/((double)(2*i)*(2*i+1)); sum += term; }   /* x - x^3/3! + x^5/5! - .. */
    return sum;
}
static double js_cos(double x){ return js_isfinite(x) ? js_sin(x + 1.5707963267948966) : JS_NAN; }   /* cos x = sin(x + pi/2) */
static double js_tan(double x){ double c=js_cos(x); return c==0.0 ? JS_NAN : js_sin(x)/c; }
static double js_atan(double x){             /* arctan, result in (-pi/2, pi/2) */
    if (js_isnan(x)) return JS_NAN;
    if (js_isinf(x)) return x<0 ? -1.5707963267948966 : 1.5707963267948966;
    int neg=x<0; if(neg) x=-x;
    /* halve the argument until small (atan(x)=2*atan(x/(1+sqrt(1+x^2)))) so the Taylor
     * series converges fast — a plain series at x~1 (Leibniz) is far too slow. */
    int halvings=0; while (x > 0.2 && halvings < 60){ x = x/(1.0 + js_sqrt(1.0 + x*x)); halvings++; }
    double term=x, sum=x, x2=x*x;
    for (int i=1;i<=14;i++){ term *= -x2; sum += term/(2*i+1); }   /* x - x^3/3 + x^5/5 - .. */
    while (halvings-- > 0) sum *= 2.0;
    return neg ? -sum : sum;
}
static double js_asin(double x){ if (x<-1.0||x>1.0) return JS_NAN; if (x==1.0) return 1.5707963267948966; if (x==-1.0) return -1.5707963267948966; return js_atan(x/js_sqrt(1.0-x*x)); }
static double js_acos(double x){ if (x<-1.0||x>1.0) return JS_NAN; return 1.5707963267948966 - js_asin(x); }

/* ---- double -> string (verbatim from js.c num_to_str, but writing into file-static
 * buffers instead of aalloc; bodies otherwise unchanged) ---- */
static char *d_i64_to_str(int64_t v) {       /* was i64_to_str: aalloc -> static buf */
    static char s[24];
    char tmp[24]; int i=0; int neg = v<0; uint64_t u = neg?(uint64_t)(-(v+1))+1:(uint64_t)v;
    if (u==0) tmp[i++]='0';
    while (u){ tmp[i++]='0'+(int)(u%10); u/=10; }
    if (neg) tmp[i++]='-';
    for(int j=0;j<i;j++) s[j]=tmp[i-1-j]; s[i]=0; return s;
}
/* double -> string, JS Number-to-string-ish: integers print exactly; otherwise up to 16
 * significant digits with trailing zeros trimmed; NaN/Infinity spelled out. Not a full
 * shortest-round-trip dtoa, but it renders the common cases (3.14, 0.5, 2/3, 1e21) cleanly. */
static char *dnum_to_str(double d){          /* was num_to_str: aalloc(40) -> static buf */
    static char buf[40];
    if (js_isnan(d)) return "NaN";
    if (js_isinf(d)) return d<0 ? "-Infinity" : "Infinity";
    if (d==0.0) return "0";                                              /* String(-0) === "0" */
    if (d==js_trunc(d) && d>=-9.0e18 && d<=9.0e18) return d_i64_to_str((int64_t)d);   /* integral & exact */
    int p=0;
    double x=d; if (x<0){ buf[p++]='-'; x=-x; }
    int e=0; double t=x;                                                 /* normalize: t in [1,10), value = t*10^e */
    while (t>=10.0){ t*=0.1; e++; } while (t<1.0){ t*=10.0; e--; }
    char dig[18]; int nd=0;                                              /* 15 significant digits via ONE integer scaling (stays < 2^53 -> exact; avoids the per-digit (t-c)*10 drift that printed 9876.5 as 9876.500000000002) */
    double sc = js_floor(t * 1e14 + 0.5);                                /* t in [1,10) -> a 15-digit integer */
    if (sc >= 1e15) { sc *= 0.1; e++; }                                  /* rounded up to 10.x -> renormalize */
    int64_t iv = (int64_t)sc;
    { char tmp[20]; int tn=0; int64_t z=iv; if(z==0) tmp[tn++]='0'; while(z){ tmp[tn++]=(char)('0'+(int)(z%10)); z/=10; } while(tn<15) tmp[tn++]='0'; for(int i=tn-1;i>=0;i--) dig[nd++]=tmp[i]; }
    while (nd>1 && dig[nd-1]=='0') nd--;                                 /* trim trailing zeros */
    if (e< -6 || e>=21){                                                 /* exponential notation */
        buf[p++]=dig[0];
        if (nd>1){ buf[p++]='.'; for(int i=1;i<nd;i++) buf[p++]=dig[i]; }
        buf[p++]='e'; buf[p++]=(e<0)?'-':'+'; int ae=e<0?-e:e;
        char eb[4]; int en=0; if(ae==0) eb[en++]='0'; while(ae){ eb[en++]=(char)('0'+ae%10); ae/=10; }
        while(en>0) buf[p++]=eb[--en];
    } else if (e>=0){                                                    /* d d d . d d  (point after e+1 digits) */
        for (int i=0;i<=e;i++) buf[p++]= (i<nd)?dig[i]:'0';
        if (nd>e+1){ buf[p++]='.'; for (int i=e+1;i<nd;i++) buf[p++]=dig[i]; }
    } else {                                                             /* 0.00..ddd  (-1>=e>=-6) */
        buf[p++]='0'; buf[p++]='.';
        for (int i=0;i<(-e-1);i++) buf[p++]='0';
        for (int i=0;i<nd;i++) buf[p++]=dig[i];
    }
    buf[p]=0; return buf;
}

#endif /* DMATH_H */
