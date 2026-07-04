/*
 * barrett_fuzz_test.c — property-based regression for the M1536 Barrett
 * reduction path: bn_barrett_init/bn_barrett_reduce directly, and bn_modexp
 * (now internally Barrett-accelerated) end-to-end. Checked against Python's
 * arbitrary-precision integers, same independent-oracle approach as
 * bignum_fuzz_test.c (bn_mod). A bug in either is a security bug (a
 * certificate could wrongly verify), not a visual glitch, so this exists
 * specifically because Barrett reduction is new, hand-derived arithmetic
 * (Knuth-shape quotient estimation), not a simple algebraic identity -- it
 * needs its own oracle-checked coverage, not just "the existing bn_mod
 * vectors still pass" (bn_mod itself is untouched).
 *
 * #includes the real bignum.c (like bignum_fuzz_test.c does) to reach the
 * Barrett internals directly. Build under ASan+UBSan via
 * tests/run-barrett-fuzz-tests.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/bignum.c"

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    return (c | 32) - 'a' + 10;
}

static void bn_from_hex(bignum *b, const char *hex) {
    uint8_t buf[BN_LIMBS * 4];
    size_t len = strlen(hex) / 2;
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)((hexval(hex[2 * i]) << 4) | hexval(hex[2 * i + 1]));
    bn_from_bytes(b, buf, len);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <vectors-file>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("fopen"); return 1; }

    char tag[4], c1[2100], c2[2100], c3[2100], c4[2100];
    int totalR = 0, failR = 0, totalE = 0, failE = 0;
    for (;;) {
        if (fscanf(f, "%3s", tag) != 1) break;
        if (tag[0] == 'R') {
            if (fscanf(f, "%2099s %2099s %2099s", c1, c2, c3) != 3) break;
            bignum a, m, want, got; bn_barrett ctx;
            bn_from_hex(&a, c1); bn_from_hex(&m, c2); bn_from_hex(&want, c3);
            bn_barrett_init(&ctx, &m);
            bn_barrett_reduce(&got, &a, &m, &ctx);
            totalR++;
            if (bn_cmp(&got, &want) != 0) {
                failR++;
                if (failR <= 10) fprintf(stderr, "  FAIL(reduce): a=%s m=%s -> mismatch (expected %s)\n", c1, c2, c3);
            }
        } else if (tag[0] == 'E') {
            if (fscanf(f, "%2099s %2099s %2099s %2099s", c1, c2, c3, c4) != 4) break;
            bignum base, exp, m, want, got;
            bn_from_hex(&base, c1); bn_from_hex(&exp, c2); bn_from_hex(&m, c3); bn_from_hex(&want, c4);
            bn_modexp(&got, &base, &exp, &m);
            totalE++;
            if (bn_cmp(&got, &want) != 0) {
                failE++;
                if (failE <= 10) fprintf(stderr, "  FAIL(modexp): base=%s exp=%s m=%s -> mismatch (expected %s)\n", c1, c2, c3, c4);
            }
        } else {
            fprintf(stderr, "FAIL: unknown vector tag '%s'\n", tag); return 1;
        }
    }
    fclose(f);

    if (totalR == 0 || totalE == 0) { fprintf(stderr, "FAIL: expected both R and E vectors, got %d/%d\n", totalR, totalE); return 1; }
    if (failR || failE) {
        printf("FAIL: %d/%d Barrett-reduce + %d/%d bn_modexp vectors mismatched\n", failR, totalR, failE, totalE);
        return 1;
    }
    printf("PASS: Barrett reduce matches the Python oracle on all %d vectors; bn_modexp matches on all %d vectors\n",
           totalR, totalE);
    return 0;
}
