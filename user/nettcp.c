// nettcp.c — exercise the Plan 9 /net/tcp sockets-as-files interface (M1110):
// clone a connection, connect to example.com:80, send a proper HTTP/1.0 request,
// and print the first line of the response — all through ordinary file I/O.
#include "ulib.h"

/* Build "/net/tcp/<idx><suffix>" -- the clone always gives back the real
 * slot, but every path used to hardcode "0" instead (only "worked" because
 * this is the sole client and slot 0 happens to be free). */
static void mkpath(char *out, const char *idx, const char *suffix) {
    const char *pfx = "/net/tcp/"; int p = 0;
    while (pfx[p]) { out[p] = pfx[p]; p++; }
    int i = 0; while (idx[i]) out[p++] = idx[i++];
    i = 0; while (suffix[i]) out[p++] = suffix[i++];
    out[p] = 0;
}

int main(void) {
    char idx[8];
    long n = sys_readfile("/net/tcp/clone", idx, sizeof idx - 1);   // clone -> slot index
    if (n <= 0) { print("nettcp: clone failed\n"); sys_sleep(2000); return 1; }
    idx[n] = 0;
    char ctlpath[24], datapath[24];
    mkpath(ctlpath, idx, "/ctl");
    mkpath(datapath, idx, "/data");
    sys_setcolor(4); print("nettcp:"); sys_setcolor(0); print(" cloned a connection via /net/tcp/clone (slot "); print(idx); print(")\n");

    if (sys_writefile(ctlpath, "connect example.com:80", 22) < 0) {
        print("nettcp: connect (via .../ctl) failed\n"); sys_sleep(2000); return 1;
    }
    print("nettcp: connected to example.com:80 via .../ctl\n");

    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    int rl = 0; while (req[rl]) rl++;
    sys_writefile(datapath, req, rl);                                 // send via .../data

    char resp[1024];
    long r = sys_readfile(datapath, resp, sizeof resp - 1);           // recv via .../data
    if (r <= 0) { print("nettcp: no response\n"); }
    else {
        resp[r] = 0;
        print("nettcp: response first line: ");
        for (long i = 0; i < r && resp[i] != '\r' && resp[i] != '\n'; i++) { char c[2] = { resp[i], 0 }; print(c); }
        print("\n");
    }
    sys_writefile(ctlpath, "close", 5);
    sys_setcolor(9); print("nettcp: closed the connection (done)\n"); sys_setcolor(0);
    sys_sleep(20000);
    return 0;
}
