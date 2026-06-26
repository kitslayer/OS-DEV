// httpd.c — a tiny in-guest HTTP server (M1133; styled page M1305; live file
// index M1306; LIVE dashboard M1307; serves REQUESTED FILES M1327). It LISTENS
// on TCP port 80 and serves pages via the from-scratch TCP server — now split
// into sys_tcp_accept() (passive-open + read the request) and sys_tcp_respond()
// (reply + close), so the response is chosen PER REQUEST. Reachable from the
// host via QEMU user-net port forwarding (`-netdev user,hostfwd=tcp::18080-:80`),
// so `curl http://localhost:18080/<path>` fetches from inside the OS.
//
//   GET /            -> a LIVE dashboard (uptime + free memory + file index),
//                       rebuilt on every request from the OS's own /proc + dir.
//   GET /FILENAME    -> the contents of that root file (e.g. /README.TXT), with
//                       a content-type guessed from the extension, or 404.
//
// The request path is parsed in USERSPACE and sanitized to ROOT FILES ONLY: any
// path containing '/', '\\' or ".." after the leading slash is rejected, so a
// remote client cannot traverse out of the FAT32 root. sys_readfile() bounds the
// read into a fixed buffer. /proc + 8.3 names carry no HTML metacharacters.
#include "ulib.h"

static int app(char *dst, int p, int cap, const char *s) {   // bounded append
    for (int i = 0; s[i] && p < cap - 1; i++) dst[p++] = s[i];
    return p;
}

// Build the full HTTP response (status line + headers + live HTML body) into
// `resp`; returns its length. Re-reads /proc + the directory each call.
static int build_page(char *resp, int cap) {
    static char body[5120];
    int b = 0;
    b = app(body, b, sizeof body,
        "<!DOCTYPE html><html><head><title>OS-DEV</title>"
        "<style>body{font-family:sans-serif;max-width:42em;margin:2.5em auto;"
        "padding:0 1.2em;color:#243;line-height:1.55}"
        "h1{color:#1f56c6;margin-bottom:.2em}h2{color:#1f56c6;margin-top:1.4em}"
        ".tag{display:inline-block;background:#1f56c6;color:#fff;padding:2px 9px;"
        "border-radius:11px;font-size:.78em;vertical-align:middle}"
        "code{background:#eef1fb;padding:1px 5px;border-radius:3px}"
        "a{color:#1f56c6}"
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
    // live root listing (the same names `ls` prints) -- each file name is a
    // clickable link that fetches it through the per-request file server (M1328).
    // FAT32 8.3 names contain only [A-Z0-9._], so they need no URL/HTML escaping.
    static char ls[1300];
    long ln = sys_list(ls, sizeof ls - 1);
    if (ln > 0) {
        ls[ln] = 0;
        int i = 0;
        while (ls[i]) {
            int eol = i; while (ls[eol] && ls[eol] != '\n') eol++;
            int ne = i; while (ne < eol && ls[ne] != ' ') ne++;        // first token = the name
            int isdir = (ne > i && ls[ne - 1] == '/');                 // dirs aren't fetchable
            if (!isdir && ne > i) {
                b = app(body, b, sizeof body, "<a href=\"/");
                for (int k = i; k < ne && b < (int)sizeof body - 1; k++) body[b++] = ls[k];
                b = app(body, b, sizeof body, "\">");
                for (int k = i; k < ne && b < (int)sizeof body - 1; k++) body[b++] = ls[k];
                b = app(body, b, sizeof body, "</a>");
            } else {
                for (int k = i; k < ne && b < (int)sizeof body - 1; k++) body[b++] = ls[k];
            }
            for (int k = ne; k < eol && b < (int)sizeof body - 1; k++) body[b++] = ls[k];   // size, etc.
            if (ls[eol] == '\n' && b < (int)sizeof body - 1) body[b++] = '\n';
            i = (ls[eol] == '\n') ? eol + 1 : eol;
        }
    }
    b = app(body, b, sizeof body,
        "</pre><p>Fetch any file above by name, e.g. <code>/README.TXT</code>.</p>"
        "<footer>OS-DEV &middot; from-scratch x86-64 OS &middot; "
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

// Parse the request-target from "GET /path HTTP/...". Copies the path (with its
// leading '/') into name[]. Returns 1 for the root "/" (=> serve the dashboard),
// else 0 with name[] = the requested file ("/FILE"), or name[]="" if the path is
// rejected as unsafe (traversal / subdir / backslash) => the caller 404s.
static int parse_path(const char *req, char *name, int cap) {
    name[0] = 0;
    const char *p = req;
    while (*p && *p != ' ') p++;                 // skip the method (GET/HEAD/...)
    while (*p == ' ') p++;
    if (*p != '/') return 0;                     // malformed request-target
    int j = 0;
    while (*p && *p != ' ' && *p != '?' && *p != '\r' && *p != '\n' && j < cap - 1)
        name[j++] = *p++;
    name[j] = 0;
    if (name[0] == '/' && name[1] == 0) return 1;            // "/" -> dashboard
    for (int i = 1; name[i]; i++)                           // root files only
        if (name[i] == '/' || name[i] == '\\' ||
            (name[i] == '.' && name[i + 1] == '.')) { name[0] = 0; return 0; }
    return 0;                                                // "/FILE"
}

// Case-insensitive ".SUFFIX" test on an 8.3 name.
static int sfx(const char *s, const char *suf) {
    int ls = 0; while (s[ls]) ls++;
    int lf = 0; while (suf[lf]) lf++;
    if (lf > ls) return 0;
    const char *a = s + ls - lf;
    for (int i = 0; i < lf; i++) {
        char c = a[i], d = suf[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (d >= 'a' && d <= 'z') d -= 32;
        if (c != d) return 0;
    }
    return 1;
}

static const char *content_type(const char *name) {
    if (sfx(name, ".htm") || sfx(name, ".html")) return "text/html";
    if (sfx(name, ".svg"))  return "image/svg+xml";
    if (sfx(name, ".css"))  return "text/css";
    if (sfx(name, ".js"))   return "text/javascript";
    if (sfx(name, ".json")) return "application/json";
    if (sfx(name, ".png"))  return "image/png";
    if (sfx(name, ".bmp"))  return "image/bmp";
    return "text/plain";
}

// Build a response serving root file `name` ("/FILE"), or a 404 if unreadable
// or the path was rejected (name[0]==0). Returns the response length.
static int serve_file(char *resp, int cap, const char *name) {
    static char fbuf[6144];
    long fn = name[0] ? sys_readfile(name, fbuf, sizeof fbuf - 1) : -1;
    int p = 0;
    if (fn <= 0) {
        p = app(resp, p, cap,
            "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n"
            "Connection: close\r\n\r\n404 Not Found: ");
        p = app(resp, p, cap, name[0] ? name : "(bad path)");
        p = app(resp, p, cap, "\n");
        return p;
    }
    p = app(resp, p, cap, "HTTP/1.0 200 OK\r\nContent-Type: ");
    p = app(resp, p, cap, content_type(name));
    p = app(resp, p, cap, "\r\nContent-Length: ");
    char num[12]; int ni = 0; long t = fn;
    if (!t) num[ni++] = '0';
    while (t) { num[ni++] = (char)('0' + t % 10); t /= 10; }
    while (ni && p < cap - 1) resp[p++] = num[--ni];
    p = app(resp, p, cap, "\r\nConnection: close\r\n\r\n");
    for (long i = 0; i < fn && p < cap - 1; i++) resp[p++] = fbuf[i];
    return p;
}

int main(void) {
    print("httpd: listening on TCP port 80\n");
    print("httpd: curl the host-forwarded port (e.g. http://localhost:18080/)\n");
    print("httpd: GET / for a live dashboard, or /FILENAME (e.g. /README.TXT) to fetch a file\n\n");

    static char resp[8192];
    static char req[1024];
    static char name[96];
    int served = 0;
    for (int k = 0; k < 400; k++) {                  // long-lived; killable between calls
        long r = sys_tcp_accept(80, req, sizeof req - 1);
        if (r <= 0) continue;                        // listen window expired -> retry
        req[r] = 0;
        int resplen;
        if (parse_path(req, name, sizeof name))
            resplen = build_page(resp, sizeof resp);          // "/" -> live dashboard
        else
            resplen = serve_file(resp, sizeof resp, name);    // "/FILE" -> file (or 404)
        sys_tcp_respond(resp, resplen);
        print("httpd: served ");
        for (int i = 0; i < r && req[i] != '\r' && req[i] != '\n'; i++) {
            char c[2] = { req[i], 0 }; print(c);
        }
        print("\n");
        served++;
    }
    print("httpd: stopping.\n");
    sys_sleep(20000);
    return served;
}
