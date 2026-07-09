/*
 * arc_test.c — host unit tests for the archive-listing engine (user/arccore.h).
 * Pure (no syscalls), so — like tests/calc, tests/sheet, tests/plot, tests/json
 * and tests/diff — we build it for the host under ASan/UBSan and check the ZIP
 * (central-directory) and TAR (header-walk) listers against hand-built archives,
 * plus format detection. Exit 0 = all pass.
 */
#include <stdio.h>
#include <string.h>
#include "arccore.h"       /* -Iuser on the compile line */

static int fails, checks;
static void chk(int cond, const char *msg) { checks++; if (!cond) { printf("  FAIL: %s\n", msg); fails++; } }

static void w16(unsigned char *p, unsigned v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void w32(unsigned char *p, unsigned long v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }

/* build a 512-byte ustar header for (name, size, typeflag) */
static void tar_hdr(unsigned char *buf, const char *name, long size, char tf) {
    memset(buf, 0, 512);
    strncpy((char *)buf, name, 99);
    char oct[16]; snprintf(oct, sizeof oct, "%011lo", size);
    memcpy(buf + 124, oct, 11); buf[135] = ' ';
    buf[156] = (unsigned char)tf;
    memcpy(buf + 257, "ustar", 5);
}

int main(void) {
    printf("archive-listing engine tests\n");

    /* --- TAR: readme.txt(41), adir/(dir), data.txt(800) --------------------*/
    {
        unsigned char t[512 * 8]; memset(t, 0, sizeof t);
        tar_hdr(t + 0,    "readme.txt", 41, '0');    /* data occupies block @512 -> next @1024 */
        tar_hdr(t + 1024, "adir/",       0, '5');    /* dir, 0 blocks -> next @1536 */
        tar_hdr(t + 1536, "data.txt",  800, '0');    /* 2 data blocks -> next @3072 (zero) */
        int n = arc_list(t, sizeof t);
        chk(n == 3, "tar: 3 entries");
        chk(strcmp(arc_ent[0].name, "readme.txt") == 0 && arc_ent[0].size == 41 && !arc_ent[0].isdir, "tar[0] readme.txt/41");
        chk(strcmp(arc_ent[1].name, "adir/") == 0 && arc_ent[1].isdir, "tar[1] adir/ is a directory");
        chk(strcmp(arc_ent[2].name, "data.txt") == 0 && arc_ent[2].size == 800, "tar[2] data.txt/800");
    }

    /* --- ZIP: central directory + EOCD (all arc_zip reads) -----------------*/
    {
        unsigned char z[512]; memset(z, 0, sizeof z);
        struct { const char *name; unsigned long usize; } ents[2] = { { "hello.txt", 33 }, { "sub/", 0 } };
        int p = 0;
        for (int e = 0; e < 2; e++) {
            unsigned char *r = z + p;
            r[0] = 'P'; r[1] = 'K'; r[2] = 1; r[3] = 2;
            int nlen = (int)strlen(ents[e].name);
            w32(r + 24, ents[e].usize);
            w16(r + 28, nlen); w16(r + 30, 0); w16(r + 32, 0);
            memcpy(r + 46, ents[e].name, nlen);
            p += 46 + nlen;
        }
        int cdsize = p;
        unsigned char *eo = z + p;
        eo[0] = 'P'; eo[1] = 'K'; eo[2] = 5; eo[3] = 6;
        w16(eo + 10, 2); w32(eo + 12, cdsize); w32(eo + 16, 0); w16(eo + 20, 0);
        p += 22;
        int n = arc_list(z, p);
        chk(n == 2, "zip: 2 entries");
        chk(strcmp(arc_ent[0].name, "hello.txt") == 0 && arc_ent[0].size == 33, "zip[0] hello.txt/33");
        chk(strcmp(arc_ent[1].name, "sub/") == 0 && arc_ent[1].isdir, "zip[1] sub/ is a directory");
    }

    /* --- format detection --------------------------------------------------*/
    { unsigned char g[2] = { 0x1f, 0x8b }; chk(arc_detect(g, 2) == ARC_GZIP, "detect gzip"); }
    { unsigned char x[8] = { 'P', 'K', 3, 4, 0, 0, 0, 0 }; chk(arc_detect(x, 8) == ARC_ZIP, "detect zip"); }
    { unsigned char y[4] = { 'h', 'i', '!', 0 }; chk(arc_detect(y, 4) == ARC_NONE, "detect none"); }
    { chk(arc_list((const unsigned char *)"not an archive", 14) == -1, "list rejects non-archive"); }

    if (!fails) printf("PASS: %d checks, archive engine correct\n", checks);
    else printf("FAIL: %d/%d checks failed\n", fails, checks);
    return fails ? 1 : 0;
}
