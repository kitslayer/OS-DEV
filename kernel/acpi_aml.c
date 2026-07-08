/*
 * acpi_aml.c — a minimal ACPI AML (ACPI Machine Language) namespace parser (M1284).
 *
 * acpi.c parses the STATIC tables (RSDP/RSDT/XSDT/FADT/MADT) and does a crude
 * byte-scan of the DSDT for the \_S5_ poweroff package. The DSDT, though, is
 * not a table — it's a block of AML *bytecode* that, when interpreted, builds
 * the ACPI namespace (the tree of Devices, Methods, Regions, ... the firmware
 * exposes). A real OS needs an AML interpreter for PCI IRQ routing (_PRT), the
 * embedded controller, power events, etc.; almost no hobby OS has one.
 *
 * This is the first step: a NAMESPACE PARSER. It walks the DSDT's top-level
 * TermList, decoding the namespace-defining opcodes — Scope, Name, Method,
 * Device, Processor/PowerResource/ThermalZone, OpRegion, Field, Mutex, Event —
 * and recursing into Scope/Device bodies, collecting every named object. It
 * decodes the two tricky AML encodings (PkgLength and NameString) properly.
 * A real TermArg/method evaluation VM followed in M1286 (aml_eval_termarg
 * below: Store/arithmetic/logical ops/If/While/Return/method calls), driving
 * aml_eval_s5() for real ACPI poweroff — but nothing anywhere calls it against
 * _PRT specifically, so PCI IRQ routing still isn't derived from AML; that
 * narrower piece is the real remaining follow-on, not general evaluation.
 *
 * SAFE SCOPE: purely additive + read-only. aml_parse() runs once after
 * acpi_init() over the (already-mapped) DSDT; on any opcode it can't size it
 * stops that TermList rather than misalign. Nothing in the kernel depends on
 * it — it's introspection a self-test reads.
 */
#include <stdint.h>
#include "console.h"
#include "string.h" /* memset for the method-eval contexts (M1289) */
#include "acpi.h"   /* aml_* decls + the AML_* object-type enum */

#define AML_MAX 256
/* For AML_NAME: data_off = where the value DataObject lives (M1286). For
 * AML_METHOD: data_off = the method body start, body_end = its end, argc = the
 * MethodFlags arg count — so aml_eval_call can run the body on demand (M1289). */
static struct { char name[5]; uint8_t type; uint8_t argc; uint32_t data_off; uint32_t body_end; } g_obj[AML_MAX];
static int g_obj_n;
static const uint8_t *g_dsdt;       /* the DSDT base, so aml_eval_* can re-decode an object's value (M1286) */
static uint32_t       g_dsdt_len;

static void aml_add(const char *seg4, uint8_t type) {
    if (g_obj_n >= AML_MAX || !seg4[0]) return;
    for (int i = 0; i < 4; i++) g_obj[g_obj_n].name[i] = seg4[i];
    g_obj[g_obj_n].name[4] = 0;
    g_obj[g_obj_n].type = type;
    g_obj[g_obj_n].argc = 0;
    g_obj[g_obj_n].data_off = 0;
    g_obj[g_obj_n].body_end = 0;
    g_obj_n++;
}

/* PkgLength: lead byte's top 2 bits = number of following bytes (0..3). With 0,
 * the length is the low 6 bits; otherwise the low nibble plus the trailing
 * bytes. The encoded value is the package size INCLUDING the PkgLength bytes
 * (but excluding the opcode). Returns the value; *consumed = its byte count. */
static uint32_t aml_pkglen(const uint8_t *p, uint32_t *consumed) {
    uint8_t lead = p[0];
    uint32_t n = (uint32_t)(lead >> 6);
    if (n == 0) { *consumed = 1; return (uint32_t)(lead & 0x3F); }
    uint32_t len = (uint32_t)(lead & 0x0F);
    for (uint32_t i = 0; i < n; i++) len |= (uint32_t)p[1 + i] << (4 + 8 * i);
    *consumed = 1 + n;
    return len;
}

/* NameString: optional '\'/'^' prefixes, then NullName (0x00) | DualNamePrefix
 * (0x2E)+2 segs | MultiNamePrefix (0x2F)+count+segs | a single 4-byte NameSeg.
 * Captures the LAST NameSeg into out[5] (enough to identify the object).
 * Returns bytes consumed. */
static uint32_t aml_namestring(const uint8_t *p, char out[5]) {
    uint32_t c = 0;
    while (p[c] == '\\' || p[c] == '^') c++;          /* root / parent prefixes */
    int nsegs;
    if (p[c] == 0x00) { c++; out[0] = 0; return c; }  /* NullName */
    if (p[c] == 0x2E)      { c++; nsegs = 2; }        /* DualNamePrefix */
    else if (p[c] == 0x2F) { c++; nsegs = p[c]; c++; }/* MultiNamePrefix + SegCount */
    else                   { nsegs = 1; }             /* a bare NameSeg */
    uint32_t segstart = c;
    if (nsegs > 0) {
        uint32_t last = segstart + (uint32_t)(nsegs - 1) * 4;
        for (int i = 0; i < 4; i++) out[i] = (char)p[last + i];
        out[4] = 0;
    } else out[0] = 0;
    return segstart + (uint32_t)nsegs * 4;
}

/* Size of a fixed DataObject (used to skip a Name's value / an OpRegion's
 * offset+length TermArgs). Handles the constant forms QEMU's DSDT uses; returns
 * 0 for anything we can't size (the caller then stops to avoid misaligning). */
static uint32_t aml_dataobj_size(const uint8_t *p, uint32_t avail) {
    if (avail < 1) return 0;
    switch (p[0]) {
        case 0x00: case 0x01: case 0xFF: return 1;          /* Zero / One / Ones */
        case 0x0A: return 2;                                /* BytePrefix  + 1 */
        case 0x0B: return 3;                                /* WordPrefix  + 2 */
        case 0x0C: return 5;                                /* DWordPrefix + 4 */
        case 0x0E: return 9;                                /* QWordPrefix + 8 */
        case 0x0D: { uint32_t n = 1; while (n < avail && p[n]) n++; return n + 1; }  /* String + NUL */
        case 0x11: case 0x12: {                             /* Buffer / Package: op + PkgLength(rest) */
            uint32_t c; uint32_t l = aml_pkglen(p + 1, &c); return 1 + l;
        }
        default: return 0;                                  /* unknown -> signal "stop" */
    }
}

static void aml_termlist(const uint8_t *aml, uint32_t p, uint32_t end, int depth) {
    if (depth > 24) return;
    while (p < end) {
        uint8_t op = aml[p];
        if (op == 0x10) {                                   /* ScopeOp PkgLength NameString TermList */
            uint32_t c; uint32_t l = aml_pkglen(aml + p + 1, &c);
            uint32_t body_end = p + 1 + l; if (body_end > end) body_end = end;
            uint32_t q = p + 1 + c; char nm[5]; q += aml_namestring(aml + q, nm);
            aml_add(nm, AML_SCOPE);
            aml_termlist(aml, q, body_end, depth + 1);
            p = p + 1 + l;
        } else if (op == 0x14) {                            /* MethodOp PkgLength NameString MethodFlags TermList */
            uint32_t c; uint32_t l = aml_pkglen(aml + p + 1, &c);
            uint32_t q = p + 1 + c; char nm[5]; q += aml_namestring(aml + q, nm);
            uint8_t flags = aml[q]; q++;                    /* MethodFlags: bits 0..2 = arg count */
            aml_add(nm, AML_METHOD);
            if (g_obj_n > 0) {                              /* record the body so aml_eval_call can run it on demand (M1289) */
                g_obj[g_obj_n - 1].argc = (uint8_t)(flags & 7);
                g_obj[g_obj_n - 1].data_off = q;            /* body start (after MethodFlags) */
                uint32_t mbody_end = p + 1 + l; if (mbody_end > end) mbody_end = end;   /* clamp to the enclosing TermList, like ScopeOp/DeviceOp already do -- a miscalculated PkgLength (a real, known firmware quirk) would otherwise let aml_eval_call read/execute past the actual DSDT buffer */
                g_obj[g_obj_n - 1].body_end = mbody_end;    /* body end */
            }
            p = p + 1 + l;                                  /* body recorded; evaluation is on demand */
        } else if (op == 0x08) {                            /* NameOp NameString DataRefObject */
            uint32_t q = p + 1; char nm[5]; q += aml_namestring(aml + q, nm);
            aml_add(nm, AML_NAME);
            if (g_obj_n > 0) g_obj[g_obj_n - 1].data_off = q;   /* remember where the value lives, for aml_eval_* (M1286) */
            uint32_t ds = aml_dataobj_size(aml + q, end - q); if (!ds) return;
            p = q + ds;
        } else if (op == 0x06) {                            /* AliasOp NameString NameString: SOURCE object, then the NEW alias name being defined */
            uint32_t q = p + 1; char a[5], b[5]; q += aml_namestring(aml + q, a); q += aml_namestring(aml + q, b);
            aml_add(b, AML_NAME); p = q;                    /* was aml_add(a,...): catalogued the pre-existing source name instead of the new alias actually being defined, dropping the real name from the namespace */
        } else if (op == 0x5B) {                            /* ExtOpPrefix */
            uint8_t ext = aml[p + 1];
            if (ext == 0x82) {                              /* DeviceOp PkgLength NameString TermList */
                uint32_t c; uint32_t l = aml_pkglen(aml + p + 2, &c);
                uint32_t body_end = p + 2 + l; if (body_end > end) body_end = end;
                uint32_t q = p + 2 + c; char nm[5]; q += aml_namestring(aml + q, nm);
                aml_add(nm, AML_DEVICE);
                aml_termlist(aml, q, body_end, depth + 1);
                p = p + 2 + l;
            } else if (ext == 0x83 || ext == 0x84 || ext == 0x85) {  /* Processor / PowerRes / ThermalZone */
                uint32_t c; uint32_t l = aml_pkglen(aml + p + 2, &c);
                char nm[5]; aml_namestring(aml + p + 2 + c, nm);
                aml_add(nm, ext == 0x83 ? AML_PROC : ext == 0x84 ? AML_POWERRES : AML_THERMAL);
                p = p + 2 + l;                              /* skip body: extra fixed fields make recursion unsafe */
            } else if (ext == 0x81) {                       /* FieldOp PkgLength NameString FieldFlags FieldList */
                uint32_t c; uint32_t l = aml_pkglen(aml + p + 2, &c);
                char nm[5]; aml_namestring(aml + p + 2 + c, nm);
                aml_add(nm, AML_FIELD);
                p = p + 2 + l;
            } else if (ext == 0x80) {                       /* OpRegionOp NameString RegionSpace RegionOffset RegionLen (no PkgLength) */
                uint32_t q = p + 2; char nm[5]; q += aml_namestring(aml + q, nm);
                aml_add(nm, AML_REGION);
                q += 1;                                     /* RegionSpace byte */
                uint32_t d1 = aml_dataobj_size(aml + q, end - q); if (!d1) return; q += d1;  /* RegionOffset */
                uint32_t d2 = aml_dataobj_size(aml + q, end - q); if (!d2) return; q += d2;  /* RegionLen */
                p = q;
            } else if (ext == 0x01) {                       /* MutexOp NameString SyncFlags */
                uint32_t q = p + 2; char nm[5]; q += aml_namestring(aml + q, nm);
                aml_add(nm, AML_MUTEX); p = q + 1;
            } else if (ext == 0x02) {                       /* EventOp NameString */
                uint32_t q = p + 2; char nm[5]; q += aml_namestring(aml + q, nm);
                aml_add(nm, AML_EVENT); p = q;
            } else return;                                  /* unknown ext op -> stop safely */
        } else return;                                      /* unknown opcode at namespace scope -> stop safely */
    }
}

/* Parse the DSDT's AML namespace. `dsdt` points at the DSDT's SDT header; the
 * AML TermList begins at offset 36 (after the 36-byte header) and runs to len. */
void aml_parse(const uint8_t *dsdt, uint32_t len) {
    g_obj_n = 0; g_dsdt = dsdt; g_dsdt_len = len;
    if (!dsdt || len <= 36) return;
    aml_termlist(dsdt, 36, len, 0);
    int dev = 0, mth = 0;
    for (int i = 0; i < g_obj_n; i++) { if (g_obj[i].type == AML_DEVICE) dev++; else if (g_obj[i].type == AML_METHOD) mth++; }
    kprintf("[ ok ] ACPI AML: parsed DSDT namespace -- %d objects (%d devices, %d methods)\n", g_obj_n, dev, mth);
}

/* Introspection for the self-test: count objects of `type` (0 = all), or check
 * whether a 4-char NameSeg is present. */
int aml_count(int type) {
    if (type == 0) return g_obj_n;
    int n = 0; for (int i = 0; i < g_obj_n; i++) if (g_obj[i].type == type) n++;
    return n;
}
int aml_has(const char *seg4) {
    for (int i = 0; i < g_obj_n; i++) {
        int m = 1; for (int k = 0; k < 4; k++) if (g_obj[i].name[k] != seg4[k]) { m = 0; break; }
        if (m) return 1;
    }
    return 0;
}
/* The i-th namespace object: fills name_out[5], returns its AML_* type (or -1 if
 * out of range). For /proc/acpi listing (M1285). */
int aml_obj(int i, char *name_out) {
    if (i < 0 || i >= g_obj_n) return -1;
    for (int k = 0; k < 5; k++) name_out[k] = g_obj[i].name[k];
    return g_obj[i].type;
}

/* aml_eval_s5 (M1286): the first real AML EVALUATION — find the \_S5_ Name in
 * the parsed namespace and decode its Package's first two integer elements
 * (SLP_TYPa, SLP_TYPb — the values written to PM1a/b_CNT to power off). This is
 * the same data acpi.c's parse_s5 extracts by a crude byte-scan; evaluating it
 * through the namespace and matching that scan is a two-decoders-agree proof.
 * Returns SLP_TYPa | (SLP_TYPb << 8), or -1 if there's no decodable _S5_. */
static long aml_eval_int_elem(const uint8_t *d, uint32_t *pp_io) {
    uint32_t pp = *pp_io; long v;
    switch (d[pp]) {
        case 0x00: v = 0; pp += 1; break;                       /* ZeroOp */
        case 0x01: v = 1; pp += 1; break;                       /* OneOp */
        case 0x0A: v = d[pp + 1]; pp += 2; break;               /* BytePrefix */
        case 0x0B: v = d[pp + 1] | (d[pp + 2] << 8); pp += 3; break;  /* WordPrefix */
        case 0x0C: v = (long)d[pp+1] | ((long)d[pp+2]<<8) | ((long)d[pp+3]<<16) | ((long)d[pp+4]<<24); pp += 5; break;
        default: return -1;                                     /* not a simple integer constant */
    }
    *pp_io = pp; return v;
}
long aml_eval_s5(void) {
    if (!g_dsdt) return -1;
    for (int i = 0; i < g_obj_n; i++) {
        if (g_obj[i].type != AML_NAME) continue;
        if (g_obj[i].name[0] != '_' || g_obj[i].name[1] != 'S' || g_obj[i].name[2] != '5' || g_obj[i].name[3] != '_') continue;
        const uint8_t *d = g_dsdt + g_obj[i].data_off;
        if (d[0] != 0x12) return -1;                            /* the value must be a Package */
        uint32_t c; (void)aml_pkglen(d + 1, &c);
        uint32_t pp = 1 + c;                                    /* skip PackageOp + PkgLength */
        uint8_t numelem = d[pp]; pp++;                          /* NumElements (a ByteData) */
        if (numelem < 2) return -1;
        long a = aml_eval_int_elem(d, &pp); if (a < 0) return -1;
        long b = aml_eval_int_elem(d, &pp); if (b < 0) return -1;
        return (a & 0xFF) | ((b & 0xFF) << 8);
    }
    return -1;
}

/* =========================================================================
 * AML method EVALUATION (M1289): a small bytecode VM over the parser above.
 *
 * The M1284 parser builds the namespace and (now) records each Method's body;
 * this turns the decoder into an INTERPRETER that actually RUNS control methods.
 * It's a recursive TermArg evaluator with 8 Locals + 7 Args per invocation and
 * the integer operation opcodes (Store/Add/Subtract/Multiply/Divide/shifts/
 * bitwise/logical), plus the statement opcodes If/Else/While/Return/Break/
 * Continue and METHOD INVOCATION (a NameString in operand position that names a
 * Method is a call — so methods can call methods, including recursively). Values
 * are 64-bit AML integers; an unsupported opcode/object (Buffer/Package/OpRegion
 * Field access, Index, ...) sets ctx->error so the evaluator fails cleanly
 * rather than misbehaving. Still purely additive + read-only of firmware AML.
 * ========================================================================= */

#define AML_NLOCAL 8
#define AML_NARG   7
#define AML_EVAL_MAXSTEPS 400000   /* runaway / infinite-While guard */
#define AML_EVAL_MAXDEPTH 24       /* method-call recursion guard */

typedef struct {
    uint64_t local[AML_NLOCAL];
    uint64_t arg[AML_NARG];
    uint64_t ret;
    int      returned;             /* ReturnOp hit -> unwind this method */
    int      brk;                  /* BreakOp hit  -> leave the loop */
    int      cont;                 /* ContinueOp   -> next loop iteration */
    int      depth;                /* call depth, for the recursion guard */
    int      error;                /* unsupported / out-of-bounds -> bail */
    long    *steps;                /* shared step budget across the call tree */
} aml_ctx;

static int aml_name4eq(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}
/* A byte that can begin a NameString (NameSeg lead, '_', or root/parent/multi). */
static int aml_is_namestart(uint8_t b) {
    return (b >= 'A' && b <= 'Z') || b == '_' || b == '\\' || b == '^' || b == 0x2E || b == 0x2F;
}

/* Method registry for the self-test: hand-assembled methods that exercise the
 * VM without depending on QEMU's (OpRegion-using) DSDT methods. Kept separate
 * from g_obj so it never disturbs the parsed firmware namespace. */
static struct { char name[5]; const uint8_t *body; uint32_t len; uint8_t argc; } g_emethod[8];
static int g_emethod_n;
void aml_eval_register(const char *name4, const uint8_t *body, uint32_t len, uint8_t argc) {
    if (g_emethod_n >= 8) return;
    for (int i = 0; i < 4; i++) g_emethod[g_emethod_n].name[i] = name4[i];
    g_emethod[g_emethod_n].name[4] = 0;
    g_emethod[g_emethod_n].body = body;
    g_emethod[g_emethod_n].len = len;
    g_emethod[g_emethod_n].argc = argc;
    g_emethod_n++;
}
/* Resolve a 4-char method name -> body bytes + arg count. Checks the self-test
 * registry first, then the parsed DSDT methods (whose bodies live in g_dsdt). */
static int aml_find_method(const char *nm, const uint8_t **body, uint32_t *len, uint8_t *argc) {
    for (int i = 0; i < g_emethod_n; i++)
        if (aml_name4eq(g_emethod[i].name, nm)) {
            *body = g_emethod[i].body; *len = g_emethod[i].len; *argc = g_emethod[i].argc; return 1;
        }
    for (int i = 0; i < g_obj_n; i++)
        if (g_obj[i].type == AML_METHOD && aml_name4eq(g_obj[i].name, nm) && g_obj[i].body_end > g_obj[i].data_off) {
            *body = g_dsdt + g_obj[i].data_off;
            *len  = g_obj[i].body_end - g_obj[i].data_off;
            *argc = g_obj[i].argc; return 1;
        }
    return 0;
}

/* A tiny runtime store for named integers a method writes to (Store(x, NAME)).
 * Reset per top-level call. Reads fall back to a parsed Name's constant value. */
static struct { char name[5]; uint64_t val; } g_nameov[16];
static int g_nameov_n;
static void aml_name_set(const char *nm, uint64_t v) {
    for (int i = 0; i < g_nameov_n; i++) if (aml_name4eq(g_nameov[i].name, nm)) { g_nameov[i].val = v; return; }
    if (g_nameov_n < 16) { for (int k = 0; k < 4; k++) g_nameov[g_nameov_n].name[k] = nm[k]; g_nameov[g_nameov_n].name[4] = 0; g_nameov[g_nameov_n].val = v; g_nameov_n++; }
}
static uint64_t aml_name_value(const char *nm) {
    for (int i = 0; i < g_nameov_n; i++) if (aml_name4eq(g_nameov[i].name, nm)) return g_nameov[i].val;
    for (int i = 0; i < g_obj_n; i++)
        if (g_obj[i].type == AML_NAME && aml_name4eq(g_obj[i].name, nm) && g_obj[i].data_off) {
            uint32_t pp = g_obj[i].data_off; long v = aml_eval_int_elem(g_dsdt, &pp);
            if (v >= 0) return (uint64_t)v;
        }
    return 0;
}

static uint64_t aml_eval_termarg(aml_ctx *cx, const uint8_t *d, uint32_t *pp, uint32_t end);
static void     aml_run(aml_ctx *cx, const uint8_t *d, uint32_t p, uint32_t end);

/* Parse a Target (SuperName) at *pp and store `val` there. A 0x00 byte is the
 * null target ("discard"). Supports Local/Arg/NameString; anything else -> error. */
static void aml_store_target(aml_ctx *cx, const uint8_t *d, uint32_t *pp, uint32_t end, uint64_t val) {
    if (cx->error || *pp >= end) { cx->error = 1; return; }
    uint8_t t = d[*pp];
    if (t == 0x00) { (*pp)++; return; }                              /* null target */
    if (t >= 0x60 && t <= 0x67) { (*pp)++; cx->local[t - 0x60] = val; return; }
    if (t >= 0x68 && t <= 0x6E) { (*pp)++; cx->arg[t - 0x68] = val; return; }
    if (aml_is_namestart(t)) { char nm[5]; *pp += aml_namestring(d + *pp, nm); aml_name_set(nm, val); return; }
    cx->error = 1;                                                   /* Index/DerefOf/... unsupported */
}

/* Evaluate a TermArg (an operand) starting at d[*pp]; advance *pp past it. */
static uint64_t aml_eval_termarg(aml_ctx *cx, const uint8_t *d, uint32_t *pp, uint32_t end) {
    if (cx->error || *pp >= end) { cx->error = 1; return 0; }
    if (++(*cx->steps) > AML_EVAL_MAXSTEPS) { cx->error = 1; return 0; }
    uint8_t op = d[(*pp)++];
    switch (op) {
        case 0x00: return 0;                                         /* Zero  */
        case 0x01: return 1;                                         /* One   */
        case 0xFF: return ~0ull;                                     /* Ones  */
        case 0x0A: { uint64_t v = d[*pp]; *pp += 1; return v; }       /* BytePrefix  */
        case 0x0B: { uint64_t v = (uint64_t)d[*pp] | ((uint64_t)d[*pp+1]<<8); *pp += 2; return v; }
        case 0x0C: { uint64_t v = 0; for (int i=0;i<4;i++) v |= (uint64_t)d[*pp+i]<<(8*i); *pp += 4; return v; }
        case 0x0E: { uint64_t v = 0; for (int i=0;i<8;i++) v |= (uint64_t)d[*pp+i]<<(8*i); *pp += 8; return v; }
        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x64: case 0x65: case 0x66: case 0x67: return cx->local[op - 0x60];
        case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x6C: case 0x6D: case 0x6E: return cx->arg[op - 0x68];
        case 0x70: {                                                 /* Store(src, dst) */
            uint64_t v = aml_eval_termarg(cx, d, pp, end); aml_store_target(cx, d, pp, end, v); return v;
        }
        /* binary integer ops: Op a b Target  (compute, store to target, return) */
        case 0x72: case 0x74: case 0x77: case 0x78: case 0x79: case 0x7A:
        case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            uint64_t a = aml_eval_termarg(cx, d, pp, end);
            if (op == 0x78) {                                        /* Divide: a b remTarget quoTarget -> quotient */
                uint64_t b = aml_eval_termarg(cx, d, pp, end);
                uint64_t q = b ? a / b : 0, rem = b ? a % b : 0;
                aml_store_target(cx, d, pp, end, rem);
                aml_store_target(cx, d, pp, end, q);
                return q;
            }
            uint64_t b = aml_eval_termarg(cx, d, pp, end), r;
            switch (op) {
                case 0x72: r = a + b; break;                         /* Add        */
                case 0x74: r = a - b; break;                         /* Subtract   */
                case 0x77: r = a * b; break;                         /* Multiply   */
                case 0x79: r = a << (b & 63); break;                 /* ShiftLeft  */
                case 0x7A: r = a >> (b & 63); break;                 /* ShiftRight */
                case 0x7B: r = a & b; break;                         /* And        */
                case 0x7C: r = ~(a & b); break;                      /* Nand       */
                case 0x7D: r = a | b; break;                         /* Or         */
                case 0x7E: r = ~(a | b); break;                      /* Nor        */
                default:   r = a ^ b; break;                         /* Xor (0x7F) */
            }
            aml_store_target(cx, d, pp, end, r);
            return r;
        }
        case 0x80: {                                                 /* Not(a, Target) */
            uint64_t a = aml_eval_termarg(cx, d, pp, end), r = ~a;
            aml_store_target(cx, d, pp, end, r); return r;
        }
        case 0x75: case 0x76: {                                      /* Increment / Decrement SuperName */
            if (*pp >= end) { cx->error = 1; return 0; }
            uint8_t t = d[*pp]; uint64_t cur, nv;
            if (t >= 0x60 && t <= 0x67) { (*pp)++; cur = cx->local[t-0x60]; nv = op==0x75?cur+1:cur-1; cx->local[t-0x60] = nv; return nv; }
            if (t >= 0x68 && t <= 0x6E) { (*pp)++; cur = cx->arg[t-0x68]; nv = op==0x75?cur+1:cur-1; cx->arg[t-0x68] = nv; return nv; }
            if (aml_is_namestart(t)) { char nm[5]; *pp += aml_namestring(d + *pp, nm); cur = aml_name_value(nm); nv = op==0x75?cur+1:cur-1; aml_name_set(nm, nv); return nv; }
            cx->error = 1; return 0;
        }
        /* logical ops: return 0 / 1, no target */
        case 0x90: { uint64_t a = aml_eval_termarg(cx,d,pp,end), b = aml_eval_termarg(cx,d,pp,end); return (a && b) ? 1 : 0; }  /* LAnd */
        case 0x91: { uint64_t a = aml_eval_termarg(cx,d,pp,end), b = aml_eval_termarg(cx,d,pp,end); return (a || b) ? 1 : 0; }  /* LOr  */
        case 0x92: { uint64_t a = aml_eval_termarg(cx,d,pp,end); return a ? 0 : 1; }                                           /* LNot */
        case 0x93: { uint64_t a = aml_eval_termarg(cx,d,pp,end), b = aml_eval_termarg(cx,d,pp,end); return (a == b) ? 1 : 0; } /* LEqual   */
        case 0x94: { uint64_t a = aml_eval_termarg(cx,d,pp,end), b = aml_eval_termarg(cx,d,pp,end); return (a >  b) ? 1 : 0; } /* LGreater */
        case 0x95: { uint64_t a = aml_eval_termarg(cx,d,pp,end), b = aml_eval_termarg(cx,d,pp,end); return (a <  b) ? 1 : 0; } /* LLess    */
        default:
            if (aml_is_namestart(op)) {                              /* NameString: method call or named-value read */
                (*pp)--;                                             /* back up: aml_namestring reads from the name's first byte */
                char nm[5]; *pp += aml_namestring(d + *pp, nm);
                const uint8_t *body; uint32_t blen; uint8_t argc;
                if (aml_find_method(nm, &body, &blen, &argc)) {      /* a CALL: gather argc args, run the body */
                    if (cx->depth >= AML_EVAL_MAXDEPTH) { cx->error = 1; return 0; }
                    aml_ctx sub; memset(&sub, 0, sizeof sub); sub.depth = cx->depth + 1; sub.steps = cx->steps;
                    for (int i = 0; i < argc && i < AML_NARG; i++) sub.arg[i] = aml_eval_termarg(cx, d, pp, end);
                    aml_run(&sub, body, 0, blen);
                    if (sub.error) cx->error = 1;
                    return sub.ret;
                }
                return aml_name_value(nm);                           /* a named integer */
            }
            cx->error = 1; return 0;                                 /* unsupported opcode */
    }
}

/* Run a TermList of STATEMENTS in [p, end) until end / Return / Break / Continue. */
static void aml_run(aml_ctx *cx, const uint8_t *d, uint32_t p, uint32_t end) {
    while (p < end && !cx->returned && !cx->error && !cx->brk && !cx->cont) {
        if (++(*cx->steps) > AML_EVAL_MAXSTEPS) { cx->error = 1; return; }
        uint8_t op = d[p];
        if (op == 0xA4) {                                            /* Return TermArg */
            uint32_t pp = p + 1; cx->ret = aml_eval_termarg(cx, d, &pp, end); cx->returned = 1; return;
        } else if (op == 0xA0) {                                     /* If PkgLength Pred TermList [Else ...] */
            uint32_t c; uint32_t l = aml_pkglen(d + p + 1, &c);
            uint32_t body_end = p + 1 + l; if (body_end > end) body_end = end;
            uint32_t pp = p + 1 + c; uint64_t pred = aml_eval_termarg(cx, d, &pp, body_end);
            uint32_t after = p + 1 + l, else_s = 0, else_e = 0;
            if (after < end && d[after] == 0xA1) {                   /* Else PkgLength TermList */
                uint32_t c2; uint32_t l2 = aml_pkglen(d + after + 1, &c2);
                else_s = after + 1 + c2; else_e = after + 1 + l2; if (else_e > end) else_e = end;
                after = after + 1 + l2;
            }
            if (!cx->error) { if (pred) aml_run(cx, d, pp, body_end); else if (else_s) aml_run(cx, d, else_s, else_e); }
            p = after;
        } else if (op == 0xA1) {                                     /* a dangling Else: skip its package */
            uint32_t c; uint32_t l = aml_pkglen(d + p + 1, &c); p = p + 1 + l;
        } else if (op == 0xA2) {                                     /* While PkgLength Pred TermList */
            uint32_t c; uint32_t l = aml_pkglen(d + p + 1, &c);
            uint32_t pred_s = p + 1 + c, body_end = p + 1 + l; if (body_end > end) body_end = end;
            for (;;) {
                if (++(*cx->steps) > AML_EVAL_MAXSTEPS) { cx->error = 1; break; }
                uint32_t pp = pred_s; uint64_t pred = aml_eval_termarg(cx, d, &pp, body_end);
                if (cx->error || !pred) break;
                cx->brk = 0; cx->cont = 0;
                aml_run(cx, d, pp, body_end);
                if (cx->returned || cx->error || cx->brk) { cx->brk = 0; break; }
                cx->cont = 0;                                        /* Continue -> just loop again */
            }
            p = p + 1 + l;
        } else if (op == 0xA5) { cx->brk = 1; return; }              /* Break    */
        else if (op == 0x9F) { cx->cont = 1; return; }               /* Continue */
        else if (op == 0xA3) { p++; }                                /* Noop     */
        else {                                                       /* expression statement (Store / op-with-target / call) */
            uint32_t pp = p; (void)aml_eval_termarg(cx, d, &pp, end);
            if (cx->error || pp <= p) { cx->error = (pp <= p) ? 1 : cx->error; return; }
            p = pp;
        }
    }
}

/* Public: evaluate method `name4` with `nargs` integer args; return its integer
 * result, or -1 if it's not found or hits an opcode/object the VM can't run. */
long aml_eval_call(const char *name4, const uint64_t *args, int nargs) {
    const uint8_t *body; uint32_t blen; uint8_t argc;
    if (!aml_find_method(name4, &body, &blen, &argc)) return -1;
    long steps = 0;
    g_nameov_n = 0;                                                  /* fresh named-store per top-level call */
    aml_ctx cx; memset(&cx, 0, sizeof cx); cx.depth = 1; cx.steps = &steps;
    for (int i = 0; i < nargs && i < AML_NARG; i++) cx.arg[i] = args[i];
    aml_run(&cx, body, 0, blen);
    return cx.error ? -1 : (long)cx.ret;
}

/* Boot self-test: prove the VM end-to-end on two hand-assembled AML methods that
 * exercise the whole feature set WITHOUT depending on hardware-touching firmware
 * methods. FACT is RECURSIVE (method invocation + If/LLess/Multiply/Subtract/
 * Return); SUMN is a LOOP (Store/While/LGreater/Add/Decrement/Return). The bytes
 * are real AML — the same encoding QEMU's DSDT uses — laid out below. */
void aml_eval_selftest(void) {
    /* Method(FACT,1){ If(LLess(Arg0,2)){Return(One)} Return(Multiply(Arg0,FACT(Subtract(Arg0,One)))) } */
    static const uint8_t fact[] = {
        0xA0, 0x07, 0x95, 0x68, 0x0A, 0x02, 0xA4, 0x01,             /* If(LLess(Arg0,2)){Return(One)} */
        0xA4, 0x77, 0x68, 0x46, 0x41, 0x43, 0x54, 0x74, 0x68, 0x01, 0x00, 0x00,
        /* Return( Multiply(Arg0, FACT(Subtract(Arg0,One)) ) ) ; 'FACT'=46 41 43 54 */
    };
    /* Method(SUMN,1){ Store(0,L0) Store(Arg0,L1) While(LGreater(L1,0)){Add(L0,L1,L0) Decrement(L1)} Return(L0) } */
    static const uint8_t sumn[] = {
        0x70, 0x00, 0x60,                                           /* Store(Zero, Local0) */
        0x70, 0x68, 0x61,                                           /* Store(Arg0, Local1) */
        0xA2, 0x0A, 0x94, 0x61, 0x00, 0x72, 0x60, 0x61, 0x60, 0x76, 0x61,  /* While(L1>0){L0+=L1; L1--} */
        0xA4, 0x60,                                                 /* Return(Local0) */
    };
    aml_eval_register("FACT", fact, sizeof fact, 1);
    aml_eval_register("SUMN", sumn, sizeof sumn, 1);
    uint64_t five = 5;
    long f = aml_eval_call("FACT", &five, 1);   /* 5! = 120 */
    long s = aml_eval_call("SUMN", &five, 1);   /* 1+2+3+4+5 = 15 */
    if (f == 120 && s == 15)
        kprintf("[ ok ] ACPI AML eval: FACT(5)=120 (recursion/If) + SUMN(5)=15 (While/Store/Add) -- AML method evaluation OK\n");
    else
        kprintf("[acpi-aml] AML EVAL FAILED: FACT(5)=%ld (want 120) SUMN(5)=%ld (want 15)\n", f, s);
}
