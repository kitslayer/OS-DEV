/*
 * webp_test.c — Host regression + round-trip test for kernel/webp.c (VP8L).
 *
 * Compile via tests/run-webp-tests.sh (ASan+UBSan).  Takes two args:
 *   argv[1] = "1" if cwebp is available, "0" otherwise
 *   argv[2] = "1" if python3 is available, "0" otherwise
 *
 * Tests:
 *   1. Malformed-input fuzzing (always runs, no external tools needed)
 *   2. Round-trip pixel accuracy (requires cwebp + python3):
 *      - gradient image
 *      - repeated-pattern image (exercises LZ77 + color cache)
 *      - few-color image (triggers color-indexing)
 * Exit 0 = all checks passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Pull in the decoder directly so ASan can instrument it */
#include "../../kernel/webp.c"

/* ---- helpers ---- */
static int fails = 0;
#define CHECK(c, msg) do { \
    if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } \
} while (0)

/* Read a file into a malloc'd buffer.  Returns NULL on error. */
static uint8_t *read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = (int)sz;
    return buf;
}

/* Read a binary PPM (P6) file.  Returns malloc'd RGB (3 bytes/pixel) buffer. */
static uint8_t *read_ppm(const char *path, int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &maxv) != 3) { fclose(f); return NULL; }
    if (w <= 0 || h <= 0 || maxv != 255) { fclose(f); return NULL; }
    /* skip one whitespace byte */
    fgetc(f);
    long n = (long)w * h * 3;
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, (size_t)n, f) != n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_w = w;
    *out_h = h;
    return buf;
}

/* RGBA output buffer (static so it doesn't blow the stack under ASan).
 * Scratch is now malloc'd to exactly scratch_need per image to prove the
 * tight estimate never under-allocates. */
static uint8_t g_rgba[4 << 20];

/* Small static scratch for malformed-input fuzz (these never get past probe) */
static uint8_t g_fuzz_scratch[4096];

/* ---- malformed-input fuzz ---- */
static void fuzz_malformed(void) {
    /* Truncated / garbage inputs must never OOB, just return non-zero */
    static const struct { const uint8_t *d; int n; const char *name; } cases[] = {
        /* empty */
        { (const uint8_t *)"", 0, "empty" },
        /* too short */
        { (const uint8_t *)"RIFF", 4, "4-byte RIFF" },
        { (const uint8_t *)"RIFF\x00\x00\x00\x00WEBP", 12, "RIFF+WEBP no chunks" },
        /* lossy chunk */
        { (const uint8_t *)"RIFF\x10\x00\x00\x00WEBPVP8 \x04\x00\x00\x00\x00\x00\x00\x00", 24, "lossy VP8" },
        /* VP8L signature wrong */
        { (const uint8_t *)"RIFF\x0C\x00\x00\x00WEBPVP8L\x04\x00\x00\x00\x00\x00\x00\x00", 24, "VP8L bad sig" },
        /* VP8L too short */
        { (const uint8_t *)"RIFF\x09\x00\x00\x00WEBPVP8L\x01\x00\x00\x00\x2F", 21, "VP8L 1-byte payload" },
        { NULL, 0, NULL }
    };

    for (int i = 0; cases[i].d; i++) {
        int w = -1, h = -1; long scr_need = 0;
        int r1 = webp_probe(cases[i].d, cases[i].n, &w, &h, &scr_need);
        int r2 = webp_decode(cases[i].d, cases[i].n,
                             g_rgba, (int)sizeof(g_rgba),
                             g_fuzz_scratch, (int)sizeof(g_fuzz_scratch),
                             &w, &h);
        /* Both must return non-zero (error) */
        char msg[128];
        snprintf(msg, sizeof(msg), "malformed '%s' probe should fail", cases[i].name);
        CHECK(r1 != 0, msg);
        snprintf(msg, sizeof(msg), "malformed '%s' decode should fail", cases[i].name);
        CHECK(r2 != 0, msg);
    }

    /* Random noise: must never crash */
    unsigned int seed = 0xDEAD;
    uint8_t noise[256];
    for (int trial = 0; trial < 2000; trial++) {
        int nlen = (seed % 200) + 1;
        seed = seed * 1103515245 + 12345;
        for (int i = 0; i < nlen; i++) {
            seed = seed * 1103515245 + 12345;
            noise[i] = (uint8_t)(seed >> 16);
        }
        int w = 0, h = 0; long sneed = 0;
        webp_probe(noise, nlen, &w, &h, &sneed);
        webp_decode(noise, nlen,
                    g_rgba, (int)sizeof(g_rgba),
                    g_fuzz_scratch, (int)sizeof(g_fuzz_scratch),
                    &w, &h);
    }

    /* NULL pointer guard */
    CHECK(webp_probe(NULL, 10, NULL, NULL, NULL) != 0, "probe(NULL) != 0");
    CHECK(webp_decode(NULL, 10, NULL, 0, NULL, 0, NULL, NULL) != 0, "decode(NULL) != 0");

    printf("malformed-input fuzz: %s\n", fails ? "FAILURES" : "ok");
}

/* ---- round-trip test helpers ---- */

/* Write a binary PPM file from RGB data (3 bytes/pixel). */
static int write_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    long n = (long)w * h * 3;
    if ((long)fwrite(rgb, 1, (size_t)n, f) != n) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Run cwebp to produce a lossless WebP from a PPM file */
static int run_cwebp(const char *in_ppm, const char *out_webp) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "cwebp -lossless -q 100 '%s' -o '%s' >/dev/null 2>&1",
             in_ppm, out_webp);
    return system(cmd);
}

/* Compare round-tripped WebP against original RGB.
 * Allocates scratch to EXACTLY scratch_need bytes (via malloc) to prove the
 * tight estimate never under-allocates.  Returns mismatching pixel count (0=pass). */
static long roundtrip_test(const char *label,
                            const uint8_t *src_rgb, int w, int h) {
    char ppm_path[128], webp_path[128];
    snprintf(ppm_path,  sizeof(ppm_path),  "/tmp/osdev_webp_test_%s.ppm",  label);
    snprintf(webp_path, sizeof(webp_path), "/tmp/osdev_webp_test_%s.webp", label);

    if (write_ppm(ppm_path, src_rgb, w, h) != 0) {
        printf("  FAIL: %s: could not write PPM\n", label);
        fails++;
        return -1;
    }
    if (run_cwebp(ppm_path, webp_path) != 0) {
        printf("  FAIL: %s: cwebp failed\n", label);
        fails++;
        return -1;
    }

    int webp_len = 0;
    uint8_t *webp_data = read_file(webp_path, &webp_len);
    if (!webp_data) {
        printf("  FAIL: %s: could not read output WebP\n", label);
        fails++;
        return -1;
    }

    int dw = 0, dh = 0; long scr_need = 0;
    int pr = webp_probe(webp_data, webp_len, &dw, &dh, &scr_need);
    if (pr != 0) {
        printf("  FAIL: %s: webp_probe returned %d\n", label, pr);
        free(webp_data);
        fails++;
        return -1;
    }
    if (dw != w || dh != h) {
        printf("  FAIL: %s: dimensions %dx%d != expected %dx%d\n", label, dw, dh, w, h);
        free(webp_data);
        fails++;
        return -1;
    }

    /* Allocate scratch to EXACTLY scratch_need — proves tight estimate is a
     * true upper bound (ASan poisons the byte immediately past this buffer). */
    uint8_t *scr = (uint8_t *)malloc((size_t)scr_need);
    if (!scr) {
        printf("  FAIL: %s: malloc(%ld) for scratch failed\n", label, scr_need);
        free(webp_data);
        fails++;
        return -1;
    }

    int ow = 0, oh = 0;
    int dr = webp_decode(webp_data, webp_len,
                         g_rgba, (int)sizeof(g_rgba),
                         scr, (int)scr_need,
                         &ow, &oh);
    free(scr);
    free(webp_data);
    if (dr != 0) {
        printf("  FAIL: %s: webp_decode returned %d\n", label, dr);
        fails++;
        return -1;
    }
    if (ow != w || oh != h) {
        printf("  FAIL: %s: decoded dimensions %dx%d != %dx%d\n", label, ow, oh, w, h);
        fails++;
        return -1;
    }

    /* Compare pixels: RGB channel only (WebP is opaque, A should be 255) */
    long mismatches = 0;
    for (long i = 0; i < (long)w * h; i++) {
        uint8_t er = src_rgb[i*3+0], eg = src_rgb[i*3+1], eb = src_rgb[i*3+2];
        uint8_t gr = g_rgba[i*4+0],  gg = g_rgba[i*4+1],  gb = g_rgba[i*4+2];
        if (er != gr || eg != gg || eb != gb) {
            if (mismatches == 0)
                printf("  FAIL: %s: first mismatch px %ld: want (%d,%d,%d) got (%d,%d,%d)\n",
                       label, i, er, eg, eb, gr, gg, gb);
            mismatches++;
        }
    }
    if (mismatches > 0) {
        printf("  FAIL: %s: %ld pixels differ\n", label, mismatches);
        fails++;
    }
    return mismatches;
}

/* ---- synthesize test images ---- */

/* 64x64 gradient */
static void make_gradient(uint8_t *rgb, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            rgb[(y*w+x)*3+0] = (uint8_t)(x * 255 / (w-1));
            rgb[(y*w+x)*3+1] = (uint8_t)(y * 255 / (h-1));
            rgb[(y*w+x)*3+2] = (uint8_t)(128);
        }
}

/* 64x64 repeated checkerboard pattern (exercises LZ77 + color cache) */
static void make_repeated(uint8_t *rgb, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int tile = ((x/8) + (y/8)) & 1;
            rgb[(y*w+x)*3+0] = tile ? 220 : 40;
            rgb[(y*w+x)*3+1] = tile ? 40 : 200;
            rgb[(y*w+x)*3+2] = tile ? 100 : 160;
        }
}

/* 64x64 few-color image (triggers color-indexing transform) */
static void make_few_colors(uint8_t *rgb, int w, int h) {
    static const uint8_t palette[4][3] = {
        {255,0,0},{0,255,0},{0,0,255},{255,255,0}
    };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int idx = ((x >> 4) + (y >> 4)) & 3;
            rgb[(y*w+x)*3+0] = palette[idx][0];
            rgb[(y*w+x)*3+1] = palette[idx][1];
            rgb[(y*w+x)*3+2] = palette[idx][2];
        }
}

/* 16-color per-pixel ramp (triggers color-indexing with bit-packing:
 * <=16 colors → 2 indices packed per byte; caught the index-mask bug). */
static void make_palette16(uint8_t *rgb, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int idx = (((x >> 2) + (y >> 2)) & 0xF);   /* 16 distinct colours */
            int r = (idx & 3) * 64;
            int g = ((idx >> 2) & 3) * 64;
            rgb[(y*w+x)*3+0] = (uint8_t)r;
            rgb[(y*w+x)*3+1] = (uint8_t)g;
            rgb[(y*w+x)*3+2] = 0;
        }
}

/* 2-colour image (triggers color-indexing with 8 indices packed per byte). */
static void make_palette2(uint8_t *rgb, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int on = (x ^ y) & 1;
            rgb[(y*w+x)*3+0] = on ? 17  : 240;
            rgb[(y*w+x)*3+1] = on ? 33  : 18;
            rgb[(y*w+x)*3+2] = on ? 200 : 9;
        }
}

/* Photographic-ish noise (defeats palette/LZ77; exercises plain literals +
 * predictor + colour transforms + longer length/distance codes). */
static void make_noise(uint8_t *rgb, int w, int h) {
    unsigned int s = 0x12345677u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            s = s * 1103515245u + 12345u; rgb[(y*w+x)*3+0] = (uint8_t)(s >> 16);
            s = s * 1103515245u + 12345u; rgb[(y*w+x)*3+1] = (uint8_t)(s >> 16);
            s = s * 1103515245u + 12345u; rgb[(y*w+x)*3+2] = (uint8_t)(s >> 16);
        }
}

/* Horizontal bands of solid colour: long single-colour runs → big LZ77
 * length codes (>= prefix 5, where the base-table bug lived). */
static void make_bands(uint8_t *rgb, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int band = y >> 3;
            rgb[(y*w+x)*3+0] = (uint8_t)(band * 20 + 5);
            rgb[(y*w+x)*3+1] = (uint8_t)(255 - band * 15);
            rgb[(y*w+x)*3+2] = (uint8_t)((band * 37) & 0xFF);
        }
}

/* Smooth diagonal gradient with mild noise.  cwebp encodes this with the
 * spatial PREDICTOR (all modes incl. avg/select/clamp) AND the cross-colour
 * transform — the combination that exposed the predictor-mode and
 * colour-channel-order bugs. */
static void make_photo(uint8_t *rgb, int w, int h) {
    unsigned int s = 0x9e3779b9u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            s = s * 1103515245u + 12345u;
            int n = (int)((s >> 18) & 0x1F) - 16;          /* small noise */
            int r = (x * 255 / (w>1?w-1:1)) + n;
            int g = (y * 255 / (h>1?h-1:1)) - n;
            int b = ((x + y) * 255 / ((w+h)>1?(w+h-2):1));
            rgb[(y*w+x)*3+0] = (uint8_t)(r<0?0:r>255?255:r);
            rgb[(y*w+x)*3+1] = (uint8_t)(g<0?0:g>255?255:g);
            rgb[(y*w+x)*3+2] = (uint8_t)(b<0?0:b>255?255:b);
        }
}

/* ---- scratch_need probe helper (prints and returns value) ---- */
static void print_probe_scratch(int w, int h) {
    int pw = 0, ph = 0; long scr = 0;
    /* synthesise a minimal valid VP8L header to feed webp_probe */
    /* RIFF<size>WEBPVP8L<size><0x2F><bits[0..3]> */
    uint8_t hdr[32];
    memset(hdr, 0, sizeof(hdr));
    /* RIFF tag */
    hdr[0]='R'; hdr[1]='I'; hdr[2]='F'; hdr[3]='F';
    /* file size (little-endian, content after byte 8) */
    int riff_sz = sizeof(hdr) - 8;
    hdr[4]=(uint8_t)riff_sz; hdr[5]=0; hdr[6]=0; hdr[7]=0;
    /* WEBP */
    hdr[8]='W'; hdr[9]='E'; hdr[10]='B'; hdr[11]='P';
    /* VP8L chunk */
    hdr[12]='V'; hdr[13]='P'; hdr[14]='8'; hdr[15]='L';
    int vp8l_sz = sizeof(hdr) - 20;
    hdr[16]=(uint8_t)vp8l_sz; hdr[17]=0; hdr[18]=0; hdr[19]=0;
    /* VP8L payload: signature 0x2F then 14+14+1+3 bits for W,H,alpha,ver */
    hdr[20] = 0x2F;
    /* Pack: W-1 in bits[0..13], H-1 in bits[14..27], alpha_used=0 in bit28, ver=0 in bits[29..31] */
    unsigned int bw = (unsigned)(w-1), bh = (unsigned)(h-1);
    uint32_t bits = (bw & 0x3FFF) | ((bh & 0x3FFF) << 14);
    hdr[21] = (uint8_t)(bits & 0xFF);
    hdr[22] = (uint8_t)((bits >> 8) & 0xFF);
    hdr[23] = (uint8_t)((bits >> 16) & 0xFF);
    hdr[24] = (uint8_t)((bits >> 24) & 0xFF);
    int r = webp_probe(hdr, (int)sizeof(hdr), &pw, &ph, &scr);
    if (r != 0 || pw != w || ph != h) {
        printf("  probe_scratch(%dx%d): probe failed (r=%d pw=%d ph=%d)\n", w, h, r, pw, ph);
        return;
    }
    printf("  scratch_need(%4dx%4d) = %7ld bytes = %.2f MB\n", w, h, scr, (double)scr / (1024.0*1024.0));
}

/* ---- main ---- */
int main(int argc, char **argv) {
    int have_cwebp   = (argc > 1 && argv[1][0] == '1');
    int have_python3 = (argc > 2 && argv[2][0] == '1');
    (void)have_python3; /* we synthesize in C directly */

    /* 0. Print scratch_need for key sizes to verify the tight estimate */
    printf("scratch_need estimates (MAX_HGROUPS=%d):\n", MAX_HGROUPS);
    print_probe_scratch(16, 16);
    print_probe_scratch(256, 256);
    print_probe_scratch(1024, 1024);

    /* 1. Malformed-input fuzz (always runs) */
    fuzz_malformed();

    /* 2. Round-trip tests (require cwebp) */
    if (!have_cwebp) {
        printf("cwebp not available — skipping round-trip tests\n");
        printf("%s\n", fails ? "FAIL" : "PASS");
        return fails ? 1 : 0;
    }

    static uint8_t src[64*64*3];
    int W = 64, H = 64;

    /* gradient */
    make_gradient(src, W, H);
    {
        long r = roundtrip_test("gradient", src, W, H);
        printf("round-trip gradient: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* repeated pattern */
    make_repeated(src, W, H);
    {
        long r = roundtrip_test("repeated", src, W, H);
        printf("round-trip repeated: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* few-color (color indexing) */
    make_few_colors(src, W, H);
    {
        long r = roundtrip_test("few_colors", src, W, H);
        printf("round-trip few_colors: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* 16-colour packed-index image (2 indices/byte) */
    make_palette16(src, W, H);
    {
        long r = roundtrip_test("palette16", src, W, H);
        printf("round-trip palette16: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* 2-colour packed-index image (8 indices/byte) */
    make_palette2(src, W, H);
    {
        long r = roundtrip_test("palette2", src, W, H);
        printf("round-trip palette2: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* photographic noise (literals + predictor + colour transform) */
    make_noise(src, W, H);
    {
        long r = roundtrip_test("noise", src, W, H);
        printf("round-trip noise: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* solid horizontal bands (long LZ77 runs, large length codes) */
    make_bands(src, W, H);
    {
        long r = roundtrip_test("bands", src, W, H);
        printf("round-trip bands: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* tiny 4x4 16-colour ramp — packed indices on a non-power-of-pack width */
    {
        static uint8_t s4[4*4*3];
        make_palette16(s4, 4, 4);
        long r = roundtrip_test("tiny4x4", s4, 4, 4);
        printf("round-trip tiny4x4: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* 1x1 single pixel */
    {
        static uint8_t s1[3] = { 200, 100, 50 };
        long r = roundtrip_test("single", s1, 1, 1);
        printf("round-trip single: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* 17x1 / 1x17 strips — exercise row/column edge handling */
    {
        static uint8_t sr[17*3];
        make_noise(sr, 17, 1);
        long r = roundtrip_test("rowstrip", sr, 17, 1);
        printf("round-trip rowstrip: %s\n", r == 0 ? "ok" : "FAIL");
        make_noise(sr, 1, 17);
        r = roundtrip_test("colstrip", sr, 1, 17);
        printf("round-trip colstrip: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* Larger images — these are big enough that cwebp picks the spatial
     * PREDICTOR (all modes) + cross-colour transform, and produces
     * single-symbol code-length trees.  They exposed the predictor-mode,
     * colour-channel-order, distance-plane and single-symbol-tree bugs. */
    {
        static uint8_t big[192*192*3];
        make_photo(big, 192, 192);
        long r = roundtrip_test("photo", big, 192, 192);
        printf("round-trip photo: %s\n", r == 0 ? "ok" : "FAIL");

        make_noise(big, 160, 160);
        r = roundtrip_test("bignoise", big, 160, 160);
        printf("round-trip bignoise: %s\n", r == 0 ? "ok" : "FAIL");

        /* odd non-square dimensions, predictor + colour transform */
        make_photo(big, 191, 97);
        r = roundtrip_test("photo_odd", big, 191, 97);
        printf("round-trip photo_odd: %s\n", r == 0 ? "ok" : "FAIL");
    }

    /* 256×256 and 1024×1024 — verify scratch_need is a true upper bound at
     * the benchmark sizes and that the new tight estimate never under-allocates.
     * Source buffers are malloc'd to avoid blowing the stack. */
    {
        uint8_t *big256 = (uint8_t *)malloc(256*256*3);
        if (big256) {
            make_photo(big256, 256, 256);
            long r = roundtrip_test("photo256", big256, 256, 256);
            printf("round-trip photo256 (256x256): %s\n", r == 0 ? "ok" : "FAIL");
            make_noise(big256, 256, 256);
            r = roundtrip_test("noise256", big256, 256, 256);
            printf("round-trip noise256 (256x256): %s\n", r == 0 ? "ok" : "FAIL");
            free(big256);
        } else {
            printf("  SKIP: malloc failed for 256x256 src buffer\n");
        }
    }
    {
        uint8_t *big1024 = (uint8_t *)malloc(1024*1024*3);
        if (big1024) {
            make_photo(big1024, 1024, 1024);
            long r = roundtrip_test("photo1024", big1024, 1024, 1024);
            printf("round-trip photo1024 (1024x1024): %s\n", r == 0 ? "ok" : "FAIL");
            free(big1024);
        } else {
            printf("  SKIP: malloc failed for 1024x1024 src buffer\n");
        }
    }

    if (fails) {
        printf("FAIL: %d check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
