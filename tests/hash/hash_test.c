/*
 * hash_test.c — host unit tests for the checksum/encoding core (user/hashcore.h).
 * Pure (no syscalls), so — like tests/calc etc. — built for the host under
 * ASan/UBSan. Checks CRC-32 against the standard check value and Base64 against
 * the RFC-4648 vectors. Exit 0 = all pass.
 */
#include <stdio.h>
#include <string.h>
#include "hashcore.h"       /* -Iuser on the compile line */

static int fails, checks;
static void cku(unsigned got, unsigned want, const char *m) { checks++; if (got != want) { printf("  FAIL %s: got %08x want %08x\n", m, got, want); fails++; } }
static void cks(const char *got, const char *want, const char *m) { checks++; if (strcmp(got, want) != 0) { printf("  FAIL %s: got \"%s\" want \"%s\"\n", m, got, want); fails++; } }

static unsigned crc(const char *s) { return hc_crc32((const unsigned char *)s, (long)strlen(s)); }
static const char *b64(const char *s) { static char o[256]; hc_b64_encode((const unsigned char *)s, (long)strlen(s), o, sizeof o); return o; }

int main(void) {
    printf("checksum/encoding core tests\n");

    /* CRC-32: the canonical check value + known cases */
    cku(crc("123456789"), 0xCBF43926u, "crc32 check value");
    cku(crc(""), 0x00000000u, "crc32 empty");
    cku(crc("The quick brown fox jumps over the lazy dog"), 0x414FA339u, "crc32 fox");
    cku(crc("a"), 0xE8B7BE43u, "crc32 'a'");

    /* Base64: the RFC-4648 padding vectors */
    cks(b64(""), "", "b64 empty");
    cks(b64("f"), "Zg==", "b64 f");
    cks(b64("fo"), "Zm8=", "b64 fo");
    cks(b64("foo"), "Zm9v", "b64 foo");
    cks(b64("foob"), "Zm9vYg==", "b64 foob");
    cks(b64("fooba"), "Zm9vYmE=", "b64 fooba");
    cks(b64("foobar"), "Zm9vYmFy", "b64 foobar");
    cks(b64("Man"), "TWFu", "b64 Man");

    /* hex32 */
    { char h[9]; hc_hex32(0xCBF43926u, h); cks(h, "cbf43926", "hex32"); }
    { char h[9]; hc_hex32(0u, h); cks(h, "00000000", "hex32 zero"); }

    if (!fails) printf("PASS: %d checks, checksum/encoding core correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", fails, checks);
    return fails ? 1 : 0;
}
