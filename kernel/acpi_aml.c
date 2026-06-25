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
 * Full TermArg/method EVALUATION (running _PRT etc.) is the follow-on; this
 * proves we can correctly decode real firmware AML by enumerating the objects
 * QEMU's DSDT defines (e.g. \_SB_.PCI0).
 *
 * SAFE SCOPE: purely additive + read-only. aml_parse() runs once after
 * acpi_init() over the (already-mapped) DSDT; on any opcode it can't size it
 * stops that TermList rather than misalign. Nothing in the kernel depends on
 * it — it's introspection a self-test reads.
 */
#include <stdint.h>
#include "console.h"
#include "acpi.h"   /* aml_* decls + the AML_* object-type enum */

#define AML_MAX 256
static struct { char name[5]; uint8_t type; uint32_t data_off; } g_obj[AML_MAX];
static int g_obj_n;
static const uint8_t *g_dsdt;       /* the DSDT base, so aml_eval_* can re-decode an object's value (M1286) */
static uint32_t       g_dsdt_len;

static void aml_add(const char *seg4, uint8_t type) {
    if (g_obj_n >= AML_MAX || !seg4[0]) return;
    for (int i = 0; i < 4; i++) g_obj[g_obj_n].name[i] = seg4[i];
    g_obj[g_obj_n].name[4] = 0;
    g_obj[g_obj_n].type = type;
    g_obj[g_obj_n].data_off = 0;
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
            uint32_t q = p + 1 + c; char nm[5]; aml_namestring(aml + q, nm);
            aml_add(nm, AML_METHOD);
            p = p + 1 + l;                                  /* skip the method body (not evaluated) */
        } else if (op == 0x08) {                            /* NameOp NameString DataRefObject */
            uint32_t q = p + 1; char nm[5]; q += aml_namestring(aml + q, nm);
            aml_add(nm, AML_NAME);
            if (g_obj_n > 0) g_obj[g_obj_n - 1].data_off = q;   /* remember where the value lives, for aml_eval_* (M1286) */
            uint32_t ds = aml_dataobj_size(aml + q, end - q); if (!ds) return;
            p = q + ds;
        } else if (op == 0x06) {                            /* AliasOp NameString NameString */
            uint32_t q = p + 1; char a[5], b[5]; q += aml_namestring(aml + q, a); q += aml_namestring(aml + q, b);
            aml_add(a, AML_NAME); p = q;
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
