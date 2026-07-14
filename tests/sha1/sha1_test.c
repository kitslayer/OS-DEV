/* sha1_test.c — host-side regression for kernel/sha1.h against published SHA-1
 * (RFC 3174 / FIPS 180) known-answer vectors, plus the exact RFC 6455 §1.3
 * WebSocket accept-key digest. Built for the host under ASan+UBSan. Exit 0 = pass. */
#include <stdio.h>
#include <string.h>
#include "sha1.h"

static int fails = 0, checks = 0;

static const char *hex(const uint8_t *d, int n) {
    static char b[64]; for (int i = 0; i < n; i++) sprintf(b + i*2, "%02x", d[i]); return b;
}
static void check(const char *msg, size_t len, const char *want) {
    checks++;
    uint8_t out[20];
    sha1_hash((const uint8_t *)msg, len, out);
    const char *got = hex(out, 20);
    if (strcmp(got, want) != 0) { printf("FAIL sha1(\"%.20s\" len=%zu): got %s want %s\n", msg, len, got, want); fails++; }
}

int main(void) {
    /* --- FIPS 180 / RFC 3174 known-answer vectors --- */
    check("", 0,          "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    check("abc", 3,       "a9993e364706816aba3e25717850c26c9cd0d89d");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");   /* the 448-bit RFC vector (spans 2 blocks) */
    /* exactly one block minus the length field (55 bytes -> single padded block) */
    check("The quick brown fox jumps over the lazy dog", 43,
                          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    check("The quick brown fox jumps over the lazy cog", 43,
                          "de9f2c7fd25e1b3afad3e85a0bd17d9b100db4b3");

    /* --- RFC 6455 §1.3: the canonical Sec-WebSocket-Accept digest input --- */
    /* SHA1("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
     * = b37a4f2cc0624f1690f64606cf385945b2bec4ea (then base64 -> s3pPLMBiTxaQ9kYGzzhZRbK+xOo=) */
    check("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
          60, "b37a4f2cc0624f1690f64606cf385945b2bec4ea");

    if (fails) { printf("sha1: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("sha1: all %d SHA-1 KAT checks passed (incl. the RFC 6455 accept-key digest)\n", checks);
    return 0;
}
