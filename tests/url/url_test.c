/* Host fuzz + regression for kernel/url.c — the URL splitter/resolver the browser
 * runs over untrusted page bytes (the address bar, <a href>, <img src>, redirect
 * Location). They write host[]/out[] into fixed caller buffers; a malformed or
 * oversized URL must never overrun (kernel-side that's a guard-page-less stack
 * OOB). We #include the real url.c and drive it with exactly-sized buffers so any
 * over-write lands in an ASan red-zone. Build under ASan+UBSan via
 * tests/run-url-tests.sh. Exit 0 = pass.
 *
 * Oracle: change url_split's `hi < hostsz - 1` bound to `hi < hostsz + 1` and
 * this aborts (ASan) — a long host then writes one past the buffer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/url.c"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

static void expect_split(const char *url, const char *want_host, const char *want_path) {
    char host[128]; const char *path = url_split(url, host, sizeof(host));
    char m[160]; snprintf(m, sizeof(m), "url_split('%s') -> host '%s' path '%s'", url, host, path);
    CHECK(strcmp(host, want_host)==0 && strcmp(path, want_path)==0, m);
}
static void expect_img(const char *base, const char *src, int want_rc, const char *want_out) {
    char out[256]; int rc = resolve_img_url(base, src, (int)strlen(src), out, sizeof(out));
    char m[200]; snprintf(m, sizeof(m), "resolve_img_url(base='%s', src='%s') rc=%d out='%s'", base, src, rc, rc?out:"");
    if (!want_rc) { CHECK(rc==0, m); return; }
    CHECK(rc==1 && strcmp(out, want_out)==0, m);
}

/* exactly-sized buffers (no slack) so any over-write red-zones under ASan */
static void fuzz_split(const char *url, int hostsz) {
    char *host = malloc(hostsz ? hostsz : 1);
    const char *path = url_split(url, host, hostsz);
    CHECK(path != NULL, "url_split returned NULL path");
    free(host);
}
static void fuzz_img(const char *base, const unsigned char *src, int srcl, int outsz) {
    char *out = malloc(outsz ? outsz : 1);
    int rc = resolve_img_url(base, (const char *)src, srcl, out, outsz);
    if (rc) CHECK(strlen(out) < (size_t)outsz, "resolve_img_url out not NUL-terminated within buffer");
    free(out);
}

int main(void) {
    /* ---- regression ---- */
    expect_split("http://example.com/path?q=1", "example.com", "/path?q=1");
    expect_split("https://a.b.c/", "a.b.c", "/");
    expect_split("https://host", "host", "/");           /* no path -> "/" */
    expect_split("nohostonly", "nohostonly", "/");        /* no scheme/path */
    expect_split("http://x.com", "x.com", "/");
    expect_img("http://e.com/d/p.html", "img.png", 1, "http://e.com/d/img.png");        /* dir-relative */
    expect_img("https://e.com/a/b", "/root.png", 1, "https://e.com/root.png");          /* root-relative */
    expect_img("http://e.com/x", "http://other.com/a.png", 1, "http://other.com/a.png");/* absolute kept */
    expect_img("https://e.com/x", "//cdn.com/a.png", 1, "https://cdn.com/a.png");        /* protocol-relative */
    expect_img("http://e.com/products/list", "?page=2", 1, "http://e.com/products/list?page=2");    /* M1772: query-only ref keeps the whole base path */
    expect_img("http://e.com/a/b?old=1", "?new=2", 1, "http://e.com/a/b?new=2");                     /* M1772: replaces the base query, keeps the path */
    expect_img("http://e.com/login?next=/acct/settings", "go.png", 1, "http://e.com/go.png");        /* M1772: a '/' in the base query is not a path separator */
    expect_img("http://e.com/x", "data:image/png;base64,AAAA", 0, 0);                    /* data: rejected */
    expect_img("http://e.com/x", "file:///etc/passwd", 0, 0);                            /* file: rejected */
    /* RFC 3986 ./ and ../ path normalization (M660): the resolved URL is canonical */
    expect_img("http://e.com/a/b/p.html", "../up.png", 1, "http://e.com/a/up.png");        /* ../ pops a segment */
    expect_img("http://e.com/a/b/c/p.html", "../../top.png", 1, "http://e.com/a/top.png"); /* two .. */
    expect_img("http://e.com/d/p.html", "./here.png", 1, "http://e.com/d/here.png");       /* ./ removed */
    expect_img("http://e.com/a/b/p.html", "../../../x.png", 1, "http://e.com/x.png");       /* .. clamped at root */
    expect_img("http://e.com/x", "http://o.com/a/../b.png", 1, "http://o.com/b.png");       /* absolute also canonicalized */
    printf("regression: %s\n", fails ? "FAILURES" : "ok (url_split + resolve_img_url)");

    /* ---- fuzz: url_split with truncations + tiny host buffers ---- */
    const char *urls[] = {
        "http://example.com/a/b/c?x=1#frag", "https://", "http://", "///", "h",
        "https://very-long-host-name-that-exceeds-small-buffers.example.com/p", "://x", "/", 0 };
    for (int i = 0; urls[i]; i++) {
        int L = (int)strlen(urls[i]);
        for (int len = 0; len <= L; len++) { char tmp[128]; memcpy(tmp,urls[i],len); tmp[len]=0; for (int hs=1; hs<=10; hs++) fuzz_split(tmp, hs); fuzz_split(tmp, 64); }
    }

    /* ---- fuzz: random URL-ish buffers through both, tiny output buffers ---- */
    srand(0x1234);
    const char *alpha = "htps:/.com?#=&abcXY01//data:file:";
    int alen = (int)strlen(alpha);
    for (int trial = 0; trial < 400000; trial++) {
        int len = rand() % 48;
        unsigned char tmp[48]; for (int i=0;i<len;i++) tmp[i] = (trial&1)?(unsigned char)alpha[rand()%alen]:(unsigned char)rand();
        char z[49]; memcpy(z,tmp,len); z[len]=0;            /* NUL-term copy for the base/url args */
        fuzz_split(z, 1 + rand()%32);
        char base[33]; int bl=rand()%32; for(int i=0;i<bl;i++) base[i]=alpha[rand()%alen]; base[bl]=0;
        fuzz_img(base, tmp, len, 1 + rand()%40);            /* src is the exactly-len (non-NUL) slice */
    }

    printf("fuzz: truncations + 400000 random URLs (tiny host/out buffers) -> %s\n", fails ? "FAILURES" : "all clean");
    if (fails) { printf("FAIL: %d check(s) failed\n", fails); return 1; }
    printf("PASS: url.c (url_split + resolve_img_url over malformed URLs, ASan/UBSan clean)\n");
    return 0;
}
