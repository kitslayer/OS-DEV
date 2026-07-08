/*
 * test_acpiaml.c — host-side regression + fuzz test for the AML namespace
 * parser (kernel/acpi_aml.c), under AddressSanitizer + UndefinedBehaviorSanitizer.
 *
 * Covers two real bugs fixed in M1685-1687/M1688: AliasOp catalogued the
 * SOURCE name instead of the alias being defined, and FieldOp never parsed
 * its FieldList at all (catalogued the OpRegion reference instead of any of
 * the actual named fields). Both are exercised as hand-built DSDT fragments,
 * checked against the public aml_has/aml_count introspection API. A fuzz pass
 * over random/truncated bytes then checks aml_parse never reads out of bounds.
 */
#include "acpi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void kprintf(const char *fmt, ...) { (void)fmt; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* AliasOp(0x06) NameString(source) NameString(alias): only the alias name
 * (the object actually being defined) should end up in the namespace. */
static void test_alias(void) {
    unsigned char dsdt[64]; memset(dsdt, 0, sizeof dsdt);
    unsigned char *b = dsdt + 36;
    int i = 0;
    b[i++] = 0x06;
    memcpy(b + i, "SRC1", 4); i += 4;   /* source object being aliased */
    memcpy(b + i, "ALS1", 4); i += 4;   /* the new alias name being defined */

    aml_parse(dsdt, 36 + i);
    CHECK(aml_count(0) == 1);
    CHECK(aml_has("ALS1") == 1);
    CHECK(aml_has("SRC1") == 0);
}

/* Field(OPR1, ByteAcc, NoLock, Preserve) { FLD1,8, Offset(...)[4 bits], FLD2,16,
 * AccessAs(...), FLD3,8 } -- the region name is a reference, not a definition;
 * FLD1/FLD2/FLD3 are the real named objects the FieldList defines. */
static int build_field_block(unsigned char *b) {
    int i = 0;
    b[i++] = 0x5B; b[i++] = 0x81;              /* ExtOpPrefix FieldOp */
    b[i++] = 0x1A;                              /* PkgLength = 26 */
    memcpy(b + i, "OPR1", 4); i += 4;           /* region NameString (reference only) */
    b[i++] = 0x00;                              /* FieldFlags */
    memcpy(b + i, "FLD1", 4); i += 4; b[i++] = 0x08;   /* NamedField, 8 bits */
    b[i++] = 0x00; b[i++] = 0x04;               /* ReservedField, 4 bits */
    memcpy(b + i, "FLD2", 4); i += 4; b[i++] = 0x10;   /* NamedField, 16 bits */
    b[i++] = 0x01; b[i++] = 0x01; b[i++] = 0x00;       /* AccessField */
    memcpy(b + i, "FLD3", 4); i += 4; b[i++] = 0x08;   /* NamedField, 8 bits */
    return i;   /* 28 */
}

static void test_field(void) {
    unsigned char dsdt[64]; memset(dsdt, 0, sizeof dsdt);
    int n = build_field_block(dsdt + 36);

    aml_parse(dsdt, 36 + n);
    CHECK(aml_count(0) == 3);
    CHECK(aml_count(AML_FIELD) == 3);
    CHECK(aml_has("OPR1") == 0);
    CHECK(aml_has("FLD1") == 1);
    CHECK(aml_has("FLD2") == 1);
    CHECK(aml_has("FLD3") == 1);
}

/* A NamedField bit-width >63 needs a multi-byte PkgLength (unlike every field
 * in test_field, which all fit the 1-byte form) -- exercise that encoding path
 * too, both for correctness and because it's the one aml_pkglen_fits has to
 * get right (n=1: a lead byte plus one trailing byte). */
static void test_field_multibyte_pkglen(void) {
    unsigned char dsdt[64]; memset(dsdt, 0, sizeof dsdt);
    unsigned char *b = dsdt + 36;
    int i = 0;
    b[i++] = 0x5B; b[i++] = 0x81;
    b[i++] = 0x0D;                              /* PkgLength = 13 */
    memcpy(b + i, "OPR3", 4); i += 4;
    b[i++] = 0x00;                              /* FieldFlags */
    memcpy(b + i, "FLD4", 4); i += 4;
    b[i++] = 0x44; b[i++] = 0x06;                /* PkgLength, 2-byte form: bit width = 100 */

    aml_parse(dsdt, 36 + i);
    CHECK(aml_count(0) == 1);
    CHECK(aml_has("FLD4") == 1);
    CHECK(aml_has("OPR3") == 0);
}

/* A FieldList that runs out of room mid-element (a truncated NamedField, or a
 * ConnectField this parser doesn't implement) must stop cleanly, not misalign
 * into whatever bytes follow. */
static void test_field_truncated(void) {
    unsigned char dsdt[64]; memset(dsdt, 0, sizeof dsdt);
    unsigned char *b = dsdt + 36;
    int i = 0;
    b[i++] = 0x5B; b[i++] = 0x81;
    b[i++] = 0x0B;                              /* PkgLength = 11 */
    memcpy(b + i, "OPR2", 4); i += 4;
    b[i++] = 0x00;                              /* FieldFlags */
    b[i++] = 0x02;                              /* ConnectField marker -- not implemented, must stop safely */
    memcpy(b + i, "XXXX", 4); i += 4;            /* would-be next field; must NOT be catalogued */

    aml_parse(dsdt, 36 + i);
    CHECK(aml_has("XXXX") == 0);
    CHECK(aml_has("OPR2") == 0);
}

/* Every prefix-truncation of a valid Field block, and single-byte corruptions
 * throughout it, must parse without an out-of-bounds access (ASan/UBSan catch
 * that directly); random buffers add broader coverage of unrelated opcodes. */
static void fuzz_no_oob(void) {
    unsigned char full[64]; memset(full, 0, sizeof full);
    int n = build_field_block(full + 36);

    for (int len = 0; len <= 36 + n; len++) aml_parse(full, (uint32_t)len);

    unsigned char buf[64];
    for (int p = 36; p < 36 + n; p++) {
        memcpy(buf, full, sizeof buf);
        for (int v = 0; v < 256; v += 23) { buf[p] = (unsigned char)v; aml_parse(buf, sizeof buf); }
    }

    srand(4242);
    for (int it = 0; it < 200000; it++) {
        int len = rand() % sizeof buf;
        for (int k = 0; k < len; k++) buf[k] = (unsigned char)rand();
        aml_parse(buf, (uint32_t)len);
    }
}

int main(void) {
    test_alias();
    test_field();
    test_field_multibyte_pkglen();
    test_field_truncated();
    fuzz_no_oob();

    if (fails) { printf("acpiamltest: %d FAILURES\n", fails); return 1; }
    printf("acpiamltest: all checks passed\n");
    return 0;
}
