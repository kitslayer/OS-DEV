// httpd.c — a tiny in-guest HTTP server (M1133). It LISTENS on TCP port 80 and
// serves a fixed page to each client, using sys_tcp_serve() — the passive-open
// counterpart of the client TCP stack. Reachable from the host via QEMU
// user-net port forwarding (`-netdev user,hostfwd=tcp::18080-:80`), so a plain
// `curl http://localhost:18080/` fetches a page generated inside the OS.
#include "ulib.h"

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

int main(void) {
    static const char *body =
        "<!DOCTYPE html><html><head><title>OS-DEV</title></head><body>"
        "<h1>Hello from OS-DEV!</h1>"
        "<p>This page is served by an in-guest HTTP server on a from-scratch "
        "x86-64 OS, over a from-scratch TCP/IP stack.</p>"
        "<p>Passive open + three-way handshake + teardown all live in "
        "kernel/net.c (M1133).</p></body></html>\n";
    int blen = slen(body);

    // assemble the response: status line + headers + body
    static char resp[2048];
    int p = 0;
    const char *h1 = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: ";
    for (int i = 0; h1[i]; i++) resp[p++] = h1[i];
    char num[12]; int ni = 0, t = blen;            // Content-Length value
    if (!t) num[ni++] = '0';
    while (t) { num[ni++] = (char)('0' + t % 10); t /= 10; }
    while (ni) resp[p++] = num[--ni];
    const char *h2 = "\r\nConnection: close\r\n\r\n";
    for (int i = 0; h2[i]; i++) resp[p++] = h2[i];
    for (int i = 0; i < blen; i++) resp[p++] = body[i];
    int resplen = p;

    print("httpd: listening on TCP port 80\n");
    print("httpd: curl the host-forwarded port (e.g. http://localhost:18080/)\n\n");

    static char req[1024];
    int served = 0;
    for (int n = 0; n < 400; n++) {                // long-lived; killable between calls
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
        // r <= 0: no client this window — keep listening
    }
    print("httpd: stopping.\n");
    sys_sleep(20000);
    return served;
}
