// httpd.c — a tiny in-guest HTTP server (M1133; styled page M1305; live file
// index M1306; LIVE dashboard M1307). It LISTENS on TCP port 80 and serves a
// page via sys_tcp_serve() — the passive-open counterpart of the client TCP
// stack. Reachable from the host via QEMU user-net port forwarding
// (`-netdev user,hostfwd=tcp::18080-:80`), so `curl http://localhost:18080/`
// fetches a page generated inside the OS.
//
// The page is rebuilt on EVERY request (inside the serve loop), so the uptime,
// free memory, and file listing are LIVE -- the response reflects the OS's state
// at the moment of the fetch, not at startup. All inputs are the OS's own /proc
// + directory data (no untrusted parsing); FAT32 8.3 names + /proc numbers carry
// no HTML metacharacters, so nothing needs escaping.
#include "ulib.h"

static int app(char *dst, int p, int cap, const char *s) {   // bounded append
    for (int i = 0; s[i] && p < cap - 1; i++) dst[p++] = s[i];
    return p;
}

// Build the full HTTP response (status line + headers + live HTML body) into
// `resp`; returns its length. Re-reads /proc + the directory each call.
static int build_page(char *resp, int cap) {
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
        "<p><span class=tag>from scratch</span> &nbsp;This page is served live by "
        "an in-guest HTTP server on a hand-built x86-64 operating system, over its "
        "own TCP/IP stack and <code>e1000</code> driver — rebuilt on every request, "
        "so the figures below are current as of this fetch.</p>"
        "<h2>System</h2><pre>");
    // live uptime (first field of /proc/uptime = seconds since boot)
    static char up[64];
    long un = sys_readfile("/proc/uptime", up, sizeof up - 1);
    if (un > 0) {
        up[un] = 0;
        for (int i = 0; up[i]; i++) if (up[i] == ' ' || up[i] == '\n') { up[i] = 0; break; }
        b = app(body, b, sizeof body, "Uptime:    "); b = app(body, b, sizeof body, up);
        b = app(body, b, sizeof body, " s\n");
    }
    // live memory (MemTotal/MemFree/MemUsed)
    static char mem[200];
    long mn = sys_readfile("/proc/meminfo", mem, sizeof mem - 1);
    if (mn > 0) { mem[mn] = 0; b = app(body, b, sizeof body, mem); }
    b = app(body, b, sizeof body, "</pre><h2>Files on this disk</h2><pre>");
    // live root listing (the same text `ls` prints)
    static char ls[1300];
    long ln = sys_list(ls, sizeof ls - 1);
    if (ln > 0) { ls[ln] = 0; b = app(body, b, sizeof body, ls); }
    b = app(body, b, sizeof body,
        "</pre><footer>OS-DEV &middot; from-scratch x86-64 OS &middot; "
        "served live over its own TCP stack</footer></body></html>\n");
    body[b] = 0;
    int blen = b;

    int p = 0;
    p = app(resp, p, cap,
        "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: ");
    char num[12]; int ni = 0, t = blen;
    if (!t) num[ni++] = '0';
    while (t) { num[ni++] = (char)('0' + t % 10); t /= 10; }
    while (ni && p < cap - 1) resp[p++] = num[--ni];
    p = app(resp, p, cap, "\r\nConnection: close\r\n\r\n");
    for (int i = 0; i < blen && p < cap - 1; i++) resp[p++] = body[i];
    return p;
}

int main(void) {
    print("httpd: listening on TCP port 80\n");
    print("httpd: curl the host-forwarded port (e.g. http://localhost:18080/)\n\n");

    static char resp[4096];
    static char req[1024];
    int served = 0;
    for (int k = 0; k < 400; k++) {                // long-lived; killable between calls
        int resplen = build_page(resp, sizeof resp);   // rebuild => live figures
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
