/* netcon.c — network debug console (M1870).
 *
 * A kernel task that serves a small line-oriented inspection shell over TCP
 * (port 2323). Its reason to exist is REAL-HARDWARE BRING-UP: on a physical
 * machine the Multiboot2 framebuffer handoff may not light up the screen (a
 * known GOP/UEFI quirk), and there may be no serial port — so the only reliable
 * window into the running kernel is the network. This console runs in the KERNEL
 * (not ring 3), so unlike a userspace app — whose print() only reaches the
 * framebuffer — it can dump kprintf/klog history, kernel state, and the PCI bus,
 * and can reboot the box.
 *
 * Transport: net_tcp_accept_open + the net_tcp_accept_recv/send/close session
 * primitives (net.c). One client at a time (shares net.c's single g_srvconn
 * slot with ws_serve/on-demand httpd — don't run those concurrently with a
 * netcon session). Between clients it blocks in the accept, which now sleeps the
 * core (srv_rx hlt) instead of spinning.
 *
 * Commands: help, dmesg, mem, ps, cpu, uptime, net, ip, pci, bcache,
 *           ls [path], cat <path>, echo <text>, reboot, quit. */

#include <stdint.h>
#include <stddef.h>
#include "netcon.h"
#include "net.h"
#include "nic.h"
#include "console.h"
#include "pmm.h"
#include "task.h"
#include "pci.h"
#include "vfs.h"
#include "bcache.h"
#include "acpi.h"
#include "smp.h"
#include "timer.h"

/* One-session-at-a-time, single kernel task => static scratch is safe and keeps
 * the task's stack small. */
static char     g_out[16384];    /* formatted command output          */
static char     g_big[65536];    /* dmesg / cat / pci capture (large) */
static uint8_t  g_rx[2048];      /* inbound bytes from the client     */
static char     g_line[1024];    /* the command line being assembled  */

/* ---- tiny buffer formatter (no snprintf in the kernel) ---------------------- */
typedef struct { char *p; char *end; } sb;
static void sb_init(sb *b, char *buf, int max) { b->p = buf; b->end = buf + max - 1; }
static void sb_str(sb *b, const char *s) { while (*s && b->p < b->end) *b->p++ = *s++; }
static void sb_ch(sb *b, char c) { if (b->p < b->end) *b->p++ = c; }
static void sb_u64(sb *b, uint64_t v) {
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i && b->p < b->end) *b->p++ = t[--i];
}
static void sb_ip(sb *b, const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) { if (i) sb_ch(b, '.'); sb_u64(b, ip[i]); }
}
static void sb_hex2(sb *b, uint8_t v) {
    const char *h = "0123456789abcdef";
    sb_ch(b, h[v >> 4]); sb_ch(b, h[v & 15]);
}
static int sb_len(sb *b, const char *buf) { return (int)(b->p - buf); }

/* Send a NUL-terminated C string on the held connection. */
static void put(const char *s) {
    int n = 0; while (s[n]) n++;
    if (n > 0) net_tcp_accept_send((const uint8_t *)s, n);
}

/* ---- command implementations ------------------------------------------------ */

static const char *BANNER =
    "\r\nOS-DEV network debug console (netcon, build " __DATE__ " " __TIME__ ") — type 'help'\r\n";
static const char *PROMPT = "netcon> ";

static const char *HELP =
    "commands:\r\n"
    "  help            this list\r\n"
    "  dmesg           kernel log ring (klog)\r\n"
    "  mem             physical memory usage\r\n"
    "  ps              task table\r\n"
    "  cpu             online CPU cores\r\n"
    "  uptime          seconds since boot\r\n"
    "  net             /proc/net (iface + ARP/DNS)\r\n"
    "  ip              our IP/gw/dns/mac\r\n"
    "  pci             enumerate the PCI bus\r\n"
    "  bcache          unified block-cache stats\r\n"
    "  ls [path]       list a directory (default /)\r\n"
    "  cat <path>      dump a file\r\n"
    "  echo <text>     echo text back (link test)\r\n"
    "  reboot          reset the machine (ACPI)\r\n"
    "  quit            close this session\r\n";

static void cmd_mem(void) {
    uint64_t total = pmm_total_bytes(), freeb = pmm_free_bytes();
    sb b; sb_init(&b, g_out, sizeof g_out);
    sb_str(&b, "phys mem: ");
    sb_u64(&b, total >> 20); sb_str(&b, " MiB total, ");
    sb_u64(&b, freeb >> 20); sb_str(&b, " MiB free, ");
    sb_u64(&b, (total - freeb) >> 20); sb_str(&b, " MiB used (");
    sb_u64(&b, total ? (total - freeb) * 100 / total : 0); sb_str(&b, "%)\r\n");
    net_tcp_accept_send((const uint8_t *)g_out, sb_len(&b, g_out));
}

static void cmd_ps(void) {
    static task_info_t ti[64];
    int n = task_snapshot(ti, 64);
    const char *st[] = { "READY", "RUN", "BLOCK", "DEAD", "STOP" };
    sb b; sb_init(&b, g_out, sizeof g_out);
    sb_str(&b, " id  state  ring   run_ms   switches\r\n");
    for (int i = 0; i < n; i++) {
        sb_ch(&b, ' ');
        sb_u64(&b, (uint64_t)ti[i].id);
        sb_str(&b, "   ");
        sb_str(&b, (ti[i].state >= 0 && ti[i].state <= 4) ? st[ti[i].state] : "?");
        sb_str(&b, "  ");
        sb_str(&b, ti[i].proc ? "ring3 " : "kernel");
        sb_str(&b, "  ");
        sb_u64(&b, ti[i].run_ms);
        sb_str(&b, "   ");
        sb_u64(&b, ti[i].nswitch);
        sb_str(&b, "\r\n");
    }
    sb_str(&b, "tasks: "); sb_u64(&b, (uint64_t)n); sb_str(&b, "\r\n");
    net_tcp_accept_send((const uint8_t *)g_out, sb_len(&b, g_out));
}

static void cmd_cpu(void) {
    sb b; sb_init(&b, g_out, sizeof g_out);
    sb_str(&b, "cpus online: "); sb_u64(&b, (uint64_t)smp_cpu_count);
    sb_str(&b, " (this core apic id "); sb_u64(&b, (uint64_t)smp_current_cpu());
    sb_str(&b, ")\r\n");
    net_tcp_accept_send((const uint8_t *)g_out, sb_len(&b, g_out));
}

static void cmd_uptime(void) {
    uint64_t t = timer_ticks();                 /* 100 Hz => 10 ms/tick */
    sb b; sb_init(&b, g_out, sizeof g_out);
    sb_str(&b, "uptime: "); sb_u64(&b, t / 100); sb_str(&b, ".");
    sb_u64(&b, (t / 10) % 10); sb_str(&b, " s\r\n");
    net_tcp_accept_send((const uint8_t *)g_out, sb_len(&b, g_out));
}

static void cmd_ip(void) {
    sb b; sb_init(&b, g_out, sizeof g_out);
    sb_str(&b, "ip  "); sb_ip(&b, net_ip());      sb_str(&b, "\r\n");
    sb_str(&b, "gw  "); sb_ip(&b, net_gateway()); sb_str(&b, "\r\n");
    sb_str(&b, "dns "); sb_ip(&b, net_dns());     sb_str(&b, "\r\n");
    sb_str(&b, "mac ");
    const uint8_t *m = net_mac();
    for (int i = 0; i < 6; i++) { if (i) sb_ch(&b, ':'); sb_hex2(&b, m[i]); }
    sb_str(&b, "\r\n");
    net_tcp_accept_send((const uint8_t *)g_out, sb_len(&b, g_out));
}

static void cmd_net(void) {
    int n = net_proc(g_big, sizeof g_big);
    if (n > 0) net_tcp_accept_send((const uint8_t *)g_big, n);
}

static void cmd_bcache(void) {
    int n = bcache_stats(g_out, sizeof g_out);
    if (n > 0) net_tcp_accept_send((const uint8_t *)g_out, n);
}

static void cmd_dmesg(void) {
    int n = klog_copy(g_big, sizeof g_big);
    if (n > 0) net_tcp_accept_send((const uint8_t *)g_big, n);
}

static void cmd_pci(void) {
    /* pci_enumerate() prints via kprintf; capture that to the socket. */
    console_capture_begin(g_big, sizeof g_big);
    pci_enumerate();
    int n = console_capture_end();
    if (n > 0) net_tcp_accept_send((const uint8_t *)g_big, n);
}

static void cmd_ls(const char *path) {
    static vfs_dirent de[128];
    if (!path || !*path) path = "/";
    int n = vfs_list_path(path, de, 128);
    if (n < 0) { put("ls: cannot list (unsupported or not found)\r\n"); return; }
    sb b; sb_init(&b, g_out, sizeof g_out);
    for (int i = 0; i < n; i++) {
        sb_str(&b, de[i].name);
        if (de[i].size) { sb_str(&b, "  ("); sb_u64(&b, de[i].size); sb_str(&b, ")"); }
        sb_str(&b, "\r\n");
        if (b.p > b.end - 96) { net_tcp_accept_send((const uint8_t *)g_out, sb_len(&b, g_out)); sb_init(&b, g_out, sizeof g_out); }
    }
    sb_str(&b, "entries: "); sb_u64(&b, (uint64_t)n); sb_str(&b, "\r\n");
    net_tcp_accept_send((const uint8_t *)g_out, sb_len(&b, g_out));
}

static void cmd_cat(const char *path) {
    if (!path || !*path) { put("cat: usage: cat <path>\r\n"); return; }
    long n = vfs_read(path, g_big, sizeof g_big - 1);
    if (n < 0) { put("cat: cannot read\r\n"); return; }
    if (n > 0) net_tcp_accept_send((const uint8_t *)g_big, (int)n);
    if (n == 0 || g_big[n - 1] != '\n') put("\r\n");
}

/* ---- command dispatch ------------------------------------------------------- */

/* Match `line` against `cmd`; if it matches (whole word), return the argument
 * (skipping one space), else NULL. */
static const char *match(const char *line, const char *cmd) {
    while (*cmd && *line == *cmd) { line++; cmd++; }
    if (*cmd) return NULL;                       /* cmd not fully consumed */
    if (*line == 0) return line;                 /* exact match, empty arg */
    if (*line != ' ') return NULL;               /* prefix of a longer word */
    while (*line == ' ') line++;
    return line;                                 /* the argument */
}

/* Execute one command line. Returns 1 to keep the session, 0 to close it. */
static int exec_line(const char *line) {
    const char *a;
    while (*line == ' ') line++;
    if (*line == 0) return 1;                     /* blank line */
    if      ((a = match(line, "help")))   put(HELP);
    else if ((a = match(line, "dmesg")))  cmd_dmesg();
    else if ((a = match(line, "mem")))    cmd_mem();
    else if ((a = match(line, "ps")))     cmd_ps();
    else if ((a = match(line, "cpu")))    cmd_cpu();
    else if ((a = match(line, "uptime"))) cmd_uptime();
    else if ((a = match(line, "net")))    cmd_net();
    else if ((a = match(line, "ip")))     cmd_ip();
    else if ((a = match(line, "pci")))    cmd_pci();
    else if ((a = match(line, "bcache"))) cmd_bcache();
    else if ((a = match(line, "ls")))     cmd_ls(a);
    else if ((a = match(line, "cat")))    cmd_cat(a);
    else if ((a = match(line, "echo")))   { put(a); put("\r\n"); }
    else if ((a = match(line, "reboot"))) { put("rebooting...\r\n"); net_tcp_accept_close(); acpi_reboot(); }
    else if ((a = match(line, "quit")) || (a = match(line, "exit"))) { put("bye\r\n"); return 0; }
    else { put("unknown command: "); put(line); put(" (try 'help')\r\n"); }
    return 1;
}

/* ---- the session + accept loop --------------------------------------------- */

/* Feed a buffer of raw bytes into the line assembler, executing each complete
 * line. Returns 1 to keep the session, 0 to close it. */
static int feed(const uint8_t *buf, int n, int *ll) {
    for (int i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < n && buf[i + 1] == '\n') i++;   /* fold CRLF */
            g_line[*ll] = 0;
            int keep = exec_line(g_line);
            *ll = 0;
            if (!keep) return 0;
            put(PROMPT);
        } else if (*ll < (int)sizeof g_line - 1) {
            g_line[(*ll)++] = c;
        }
    }
    return 1;
}

static void session(void) {
    int ll = 0;
    put(BANNER);
    put(PROMPT);
    for (;;) {
        /* ~5 min idle per read; on timeout assume the client wandered off. */
        long n = net_tcp_accept_recv(g_rx, sizeof g_rx, 30000);
        if (n < 0) break;                         /* peer closed / RST */
        if (n == 0) break;                        /* idle timeout       */
        if (!feed(g_rx, (int)n, &ll)) break;      /* 'quit' */
    }
    net_tcp_accept_close();
}

void netcon_task(void) {
    /* On a netcon bring-up boot we OWN the network: net_demo's internet self-test
     * is skipped (kmain), so we are the sole nic_receive consumer — the drivers'
     * RX rings aren't built for two concurrent pollers (that races to a fault).
     * Bring the NIC up ourselves (single caller, no double-init hazard). */
    if (nic_init() != 0) {
        kprintf("[netcon] no supported NIC found — debug console unavailable\n");
        return;
    }
    /* Real hardware needs a routable address — the static SLIRP default
     * (10.0.2.15) won't work on a real LAN, and net_dhcp() is otherwise only
     * reached by the `dhcp` shell command. Best-effort DORA (we're the sole RX
     * consumer, so no contention); on failure we keep the static IP. Under QEMU
     * SLIRP this just re-derives the same address, harmless in the test too. */
    int leased = (net_dhcp() == 0);
    const uint8_t *ip = net_ip(), *m = net_mac();
    kprintf("[netcon] debug console on tcp %d.%d.%d.%d:%u  (%s, mac %02x:%02x:%02x:%02x:%02x:%02x)\n",
            ip[0], ip[1], ip[2], ip[3], (unsigned)NETCON_PORT,
            leased ? "DHCP lease" : "static/no-lease",
            m[0], m[1], m[2], m[3], m[4], m[5]);

    for (;;) {
        /* Wait (sleeping, via srv_rx's hlt) up to ~1 h for a client, then re-arm. */
        if (net_tcp_accept_open(NETCON_PORT, 360000) == 0)
            session();
    }
}
