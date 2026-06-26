// nettcp.c — exercise the Plan 9 /net/tcp sockets-as-files interface (M1110):
// clone a connection, connect to example.com:80, send a proper HTTP/1.0 request,
// and print the first line of the response — all through ordinary file I/O.
#include "ulib.h"

int main(void) {
    char idx[8];
    long n = sys_readfile("/net/tcp/clone", idx, sizeof idx - 1);   // clone -> slot index
    if (n <= 0) { print("nettcp: clone failed\n"); sys_sleep(2000); return 1; }
    sys_setcolor(4); print("nettcp:"); sys_setcolor(0); print(" cloned a connection via /net/tcp/clone\n");

    if (sys_writefile("/net/tcp/0/ctl", "connect example.com:80", 22) < 0) {
        print("nettcp: connect (via .../ctl) failed\n"); sys_sleep(2000); return 1;
    }
    print("nettcp: connected to example.com:80 via .../ctl\n");

    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    int rl = 0; while (req[rl]) rl++;
    sys_writefile("/net/tcp/0/data", req, rl);                       // send via .../data

    char resp[1024];
    long r = sys_readfile("/net/tcp/0/data", resp, sizeof resp - 1); // recv via .../data
    if (r <= 0) { print("nettcp: no response\n"); }
    else {
        resp[r] = 0;
        print("nettcp: response first line: ");
        for (long i = 0; i < r && resp[i] != '\r' && resp[i] != '\n'; i++) { char c[2] = { resp[i], 0 }; print(c); }
        print("\n");
    }
    sys_writefile("/net/tcp/0/ctl", "close", 5);
    sys_setcolor(9); print("nettcp: closed the connection (done)\n"); sys_setcolor(0);
    sys_sleep(20000);
    return 0;
}
