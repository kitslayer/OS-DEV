// httpd.c — a tiny in-guest HTTP server (M1133; styled page M1305; live file
// index M1306). It LISTENS on TCP port 80 and serves a page via sys_tcp_serve()
// — the passive-open counterpart of the client TCP stack. Reachable from the
// host via QEMU user-net port forwarding (`-netdev user,hostfwd=tcp::18080-:80`),
// so a plain `curl http://localhost:18080/` fetches a page generated inside the
// OS. The page now includes a LIVE listing of the disk's files (sys_list), built
// once at startup — FAT32 8.3 names contain no HTML metacharacters, so embedding
// them in a <pre> needs no escaping and adds no per-request parsing.
#include "ulib.h"

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int app(char *dst, int p, int cap, const char *s) {   // bounded append
    for (int i = 0; s[i] && p < cap - 1; i++) dst[p++] = s[i];
    return p;
}

int main(void) {
    // --- build the HTML body (styled landing page + a live file listing) ---
    static char body[3072];
    int b = 0;
    b = app(body, b, sizeof body,
        "<!DOCTYPE html><html><head><title>OS-DEV</title>"
        "<style>body{font-family:sans-serif;max-width:42em;margin:2.5em auto;"
        "padding:0 1.2em;color:#243;line-height:1.55}"
        "h1{color:#1f56c6;margin-bottom:.2em}h2{color:#1f56c6;margin-top:1.4em}"
        ".tag{display:inline-block;background:#1f56c6;color:#fff;padding:2px 9px;"
        "border-radius:11px;font-size:.78em;vertical-align:middle}"
        "code{background:#eef1fb;padding:1px 5px;border-radius:3px}"
        "pre{background:#f4f6fb;padding:.8em 1em;border-radius:6px;overflow:auto}"
        "footer{margin-top:2em;color:#789;font-size:.85em}</style></head><body>"
        "<h1>Hello from OS-DEV!</h1>"
        "<p><span class=tag>from scratch</span> &nbsp;This page is served by an "
        "in-guest HTTP server on a hand-built x86-64 operating system, over its "
        "own TCP/IP stack and <code>e1000</code> driver.</p>"
        "<p>The passive open, three-way handshake, and connection teardown all "
        "live in <code>kernel/net.c</code> (M1133). The same OS also has its own "
        "windowing desktop, FAT32 / ext2 filesystems, a JavaScript engine, and a "
        "web browser that fetches the real HTTPS web.</p>"
        "<h2>Files on this disk</h2><pre>");
    // the live root listing (the same text `ls` prints); FAT32 names are
    // [A-Z0-9_.] only, so no HTML escaping is needed.
    static char ls[1400];
    long n = sys_list(ls, sizeof ls - 1);
    if (n > 0) { ls[n] = 0; b = app(body, b, sizeof body, ls); }
    b = app(body, b, sizeof body,
        "</pre><footer>OS-DEV &middot; from-scratch x86-64 OS &middot; "
        "served over its own TCP stack</footer></body></html>\n");
    body[b] = 0;
    int blen = b;

    // --- assemble the response: status line + headers + body ---
    static char resp[4096];
    int p = 0;
    p = app(resp, p, sizeof resp,
        "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: ");
    char num[12]; int ni = 0, t = blen;
    if (!t) num[ni++] = '0';
    while (t) { num[ni++] = (char)('0' + t % 10); t /= 10; }
    while (ni) resp[p++] = num[--ni];
    p = app(resp, p, sizeof resp, "\r\nConnection: close\r\n\r\n");
    for (int i = 0; i < blen && p < (int)sizeof resp - 1; i++) resp[p++] = body[i];
    int resplen = p;

    print("httpd: listening on TCP port 80\n");
    print("httpd: curl the host-forwarded port (e.g. http://localhost:18080/)\n\n");

    static char req[1024];
    int served = 0;
    for (int k = 0; k < 400; k++) {                // long-lived; killable between calls
        long r = sys_tcp_serve(80, resp, resplen, req, sizeof req - 1);
        if (r > 0) {
            req[r] = 0;
            print("httpd: served request -> ");
            for (int i = 0; i < r && req[i] != '\r' && req[i] != '\n'; i++) {
                char c[2] = { req[i], 0 }; print(c);
            }
            print("\n");
            served++;
        }
    }
    print("httpd: stopping.\n");
    sys_sleep(20000);
    return served;
}
