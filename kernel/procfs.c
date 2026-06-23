/*
 * procfs.c — synthetic /proc and /dev, generated from live kernel state.
 *
 * A recognized hobby-OS / r/osdev milestone: the Unix "everything is a file"
 * idiom. Because our VFS dispatches by NAME (not fd), the vfs layer just routes
 * paths under /proc and /dev to the generators here — no per-process fd table
 * needed. Reads format live data (RAM from the PMM, uptime from the timer, the
 * CPU via CPUID, the process count) into the caller buffer; the /dev nodes are
 * the classic null / zero / random / full. Bounded string builders; read-only
 * for /proc, and /dev writes either discard (null) or fail (full).
 */
#include "procfs.h"
#include "pmm.h"
#include "vmm.h"
#include "timer.h"
#include "task.h"
#include "app.h"
#include "blockdev.h"
#include "interrupts.h"
#include "console.h"
#include "random.h"
#include "ksyms.h"
#include "net.h"
#include "fsevents.h"
#include "profile.h"
#include "mbox.h"
#include "measure.h"
#include "cas.h"
#include "fw.h"
#include "notify.h"
#include "eventfd.h"
#include "strace.h"
#include "swap.h"
#include "shm.h"
#include <stdint.h>

extern int task_count(void);   /* kernel/task.c */

/* ---- tiny bounded string/number appenders -------------------------------- */
static int sapp(char *b, int p, int max, const char *s) {
    while (*s && p < max - 1) b[p++] = *s++;
    return p;
}
static int sdec(char *b, int p, int max, uint64_t v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && p < max - 1) b[p++] = t[--n];
    return p;
}
/* two-digit fractional centiseconds for uptime "S.cc" */
static int sdec2(char *b, int p, int max, uint64_t v) {
    if (p < max - 1) b[p++] = (char)('0' + (v / 10) % 10);
    if (p < max - 1) b[p++] = (char)('0' + v % 10);
    return p;
}

static int peq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
static int startswith(const char *s, const char *pre) {
    while (*pre) { if (*s++ != *pre++) return 0; }
    return 1;
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

/* ---- /proc generators ----------------------------------------------------- */
static long gen_meminfo(char *b, int max) {
    uint64_t total = pmm_total_bytes() / 1024, freeb = pmm_free_bytes() / 1024;
    int p = 0;
    p = sapp(b, p, max, "MemTotal:     "); p = sdec(b, p, max, total); p = sapp(b, p, max, " kB\n");
    p = sapp(b, p, max, "MemFree:      "); p = sdec(b, p, max, freeb); p = sapp(b, p, max, " kB\n");
    p = sapp(b, p, max, "MemUsed:      "); p = sdec(b, p, max, total - freeb); p = sapp(b, p, max, " kB\n");
    b[p] = 0; return p;
}
static long gen_uptime(char *b, int max) {
    uint64_t ms = timer_ms();
    int p = 0;
    p = sdec(b, p, max, ms / 1000); b[p < max-1 ? p++ : p] = '.'; p = sdec2(b, p, max, (ms % 1000) / 10);
    p = sapp(b, p, max, " ");
    p = sdec(b, p, max, ms / 1000); b[p < max-1 ? p++ : p] = '.'; p = sdec2(b, p, max, (ms % 1000) / 10);
    p = sapp(b, p, max, "\n");
    b[p] = 0; return p;
}
static long gen_cpuinfo(char *b, int max) {
    uint32_t a, x, c, d;
    int p = 0;
    /* vendor string (leaf 0: EBX, EDX, ECX) */
    char vendor[13];
    cpuid(0, 0, &a, &x, &c, &d);
    *(uint32_t *)&vendor[0] = x; *(uint32_t *)&vendor[4] = d; *(uint32_t *)&vendor[8] = c;
    vendor[12] = 0;
    p = sapp(b, p, max, "vendor_id\t: "); p = sapp(b, p, max, vendor); p = sapp(b, p, max, "\n");
    /* brand string (leaves 0x80000002..4) */
    char brand[49]; int bi = 0;
    for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        cpuid(leaf, 0, &a, &x, &c, &d);
        *(uint32_t *)&brand[bi] = a; *(uint32_t *)&brand[bi+4] = x;
        *(uint32_t *)&brand[bi+8] = c; *(uint32_t *)&brand[bi+12] = d; bi += 16;
    }
    brand[48] = 0;
    const char *bp = brand; while (*bp == ' ') bp++;     /* trim leading spaces */
    p = sapp(b, p, max, "model name\t: "); p = sapp(b, p, max, bp); p = sapp(b, p, max, "\n");
    /* a few feature flags from leaf 1 */
    cpuid(1, 0, &a, &x, &c, &d);
    p = sapp(b, p, max, "flags\t\t:");
    if (d & (1u<<0))  p = sapp(b, p, max, " fpu");
    if (d & (1u<<4))  p = sapp(b, p, max, " tsc");
    if (d & (1u<<5))  p = sapp(b, p, max, " msr");
    if (d & (1u<<6))  p = sapp(b, p, max, " pae");
    if (d & (1u<<9))  p = sapp(b, p, max, " apic");
    if (d & (1u<<23)) p = sapp(b, p, max, " mmx");
    if (d & (1u<<25)) p = sapp(b, p, max, " sse");
    if (d & (1u<<26)) p = sapp(b, p, max, " sse2");
    if (c & (1u<<0))  p = sapp(b, p, max, " sse3");
    if (c & (1u<<19)) p = sapp(b, p, max, " sse4_1");
    if (c & (1u<<28)) p = sapp(b, p, max, " avx");
    if (c & (1u<<30)) p = sapp(b, p, max, " rdrand");
    p = sapp(b, p, max, "\n");
    b[p] = 0; return p;
}
static long gen_version(char *b, int max) {
    int p = sapp(b, 0, max, "OS-DEV version 1.0 (x86_64) - a from-scratch kernel, built " __DATE__ " " __TIME__ "\n");
    b[p] = 0; return p;
}
static long gen_loadavg(char *b, int max) {
    int p = sapp(b, 0, max, "0.00 0.00 0.00 1/");
    p = sdec(b, p, max, (uint64_t)task_count());
    p = sapp(b, p, max, " 0\n");
    b[p] = 0; return p;
}
static long gen_processes(char *b, int max) {
    task_info_t ti[24];
    int cnt = task_snapshot(ti, 24);
    static const char *st[5] = { "ready", "run  ", "block", "dead ", "stop " };
    int p = sapp(b, 0, max, "  PID  STATE  NAME\n");
    for (int i = 0; i < cnt; i++) {
        if (ti[i].state == 3) continue;                 /* skip dead */
        p = sapp(b, p, max, "  ");
        p = sdec(b, p, max, (uint64_t)ti[i].id);
        p = sapp(b, p, max, "    ");
        p = sapp(b, p, max, st[(unsigned)ti[i].state < 5 ? ti[i].state : 0]);
        p = sapp(b, p, max, "  ");
        p = sapp(b, p, max, ti[i].proc ? app_title((app_t *)ti[i].proc) : "(kernel)");
        p = sapp(b, p, max, "\n");
    }
    b[p] = 0; return p;
}
static long gen_partitions(char *b, int max) {   /* the block-device + FAT32-volume map (same data as `lsblk`) */
    return blockdev_format(b, max);
}
static long gen_bcache(char *b, int max) {       /* the disk buffer-cache stats (M1095) */
    return blockdev_cache_format(b, max);
}
static long gen_measure(char *b, int max) {      /* measured-boot PCRs + event log (M1096) */
    return measure_format(b, max);
}
static long gen_cas(char *b, int max) {          /* content-addressed store stats (M1097) */
    return cas_format(b, max);
}
static long gen_fw(char *b, int max) {           /* packet-filter rules + hit counts (M1100) */
    return fw_format(b, max);
}
static long gen_notify(char *b, int max) {       /* notification objects + pending masks (M1101) */
    return notify_format(b, max);
}
static long gen_events(char *b, int max) {       /* eventfd counters (M1113) */
    return eventfd_format(b, max);
}
static long gen_swaps(char *b, int max) {        /* swap device + page-out/in stats (M1105) */
    return swap_format(b, max);
}
static long gen_shm(char *b, int max) {          /* named shared-memory objects (M1108) */
    return shm_format(b, max);
}
static long gen_filesystems(char *b, int max) {
    int p = sapp(b, 0, max, "nodev\tprocfs\nnodev\tdevfs\n      \tfat32\n");
    b[p] = 0; return p;
}
static long gen_mounts(char *b, int max) {
    int p = sapp(b, 0, max,
        "fat32 / fat32 rw 0 0\n"
        "procfs /proc procfs ro 0 0\n"
        "devfs /dev devfs ro 0 0\n");
    b[p] = 0; return p;
}
static long gen_interrupts(char *b, int max) {
    static const char *names[16] = {
        "timer", "keyboard", "cascade", "COM2", "COM1", "LPT2", "floppy", "LPT1",
        "RTC", "irq9", "irq10", "irq11", "mouse", "FPU", "ATA0", "ATA1"
    };
    int p = sapp(b, 0, max, "  IRQ  COUNT       NAME\n");
    for (int i = 0; i < 16; i++) {
        uint64_t c = irq_count(i);
        if (c == 0) continue;                          /* only IRQs that have fired */
        p = sapp(b, p, max, "  "); p = sdec(b, p, max, (uint64_t)i);
        p = sapp(b, p, max, "    "); p = sdec(b, p, max, c);
        p = sapp(b, p, max, "    "); p = sapp(b, p, max, names[i]);
        p = sapp(b, p, max, "\n");
    }
    b[p] = 0; return p;
}
static long gen_stat(char *b, int max) {
    int p = sapp(b, 0, max, "processes ");
    p = sdec(b, p, max, (uint64_t)task_count());
    p = sapp(b, p, max, "\nbtime 0\nuptime_ms ");
    p = sdec(b, p, max, timer_ms());
    p = sapp(b, p, max, "\n");
    b[p] = 0; return p;
}
static long gen_kmsg(char *b, int max) {        /* the kernel log ring buffer (dmesg) */
    return klog_copy(b, max);
}
static long gen_net(char *b, int max) {         /* interface + ARP/DNS caches (Linux /proc/net-ish) */
    return net_proc(b, max);
}
static long gen_fsevents(char *b, int max) {    /* recent filesystem mutations (inotify-style, M1085) */
    return fsevents_format(b, max);
}
static long gen_profile(char *b, int max) {     /* sampling profiler histogram (M1086) */
    return prof_format(b, max);
}
static long gen_ipc(char *b, int max) {         /* named message queues + pending depth (M1087) */
    return mbox_format(b, max);
}
static long gen_binds(char *b, int max) {       /* active bind mounts (M1091) */
    return vfs_binds_format(b, max);
}
static long gen_kallsyms(char *b, int max) {    /* the embedded kernel symbol table (addr + name per line) */
    int p = 0;
    for (int i = 0; i < ksyms_count && p < max - 24; i++) {
        unsigned long a = ksyms[i].addr;
        char h[16]; int hn = 0;
        if (a == 0) h[hn++] = '0';
        while (a && hn < 16) { int d = (int)(a & 0xf); h[hn++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); a >>= 4; }
        while (hn && p < max - 1) b[p++] = h[--hn];
        if (p < max - 1) b[p++] = ' ';
        const char *n = ksyms[i].name;
        while (*n && p < max - 1) b[p++] = *n++;
        if (p < max - 1) b[p++] = '\n';
    }
    b[p] = 0; return p;
}
static long gen_sched(char *b, int max) {       /* per-task CPU time + system idle% (backs `top`) */
    task_info_t ti[24];
    int cnt = task_snapshot(ti, 24);
    uint64_t up = timer_ms(); if (up == 0) up = 1;
    uint64_t idle = task_idle_ms(); if (idle > up) idle = up;
    static const char *st[5] = { "ready", "run  ", "block", "dead ", "stop " };
    uint64_t memt = pmm_total_bytes() / (1024 * 1024), memf = pmm_free_bytes() / (1024 * 1024);
    int p = sapp(b, 0, max, "uptime_ms ");  p = sdec(b, p, max, up);
    p = sapp(b, p, max, "  idle ");         p = sdec(b, p, max, (idle * 100) / up); p = sapp(b, p, max, "%\n");
    p = sapp(b, p, max, "mem ");            p = sdec(b, p, max, memt - memf);
    p = sapp(b, p, max, "/");               p = sdec(b, p, max, memt);
    p = sapp(b, p, max, " MiB used  tasks "); p = sdec(b, p, max, (uint64_t)task_count());
    p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  PID  STATE  CPU_MS   CPU%  SWITCHES  NAME\n");
    for (int i = 0; i < cnt; i++) {
        if (ti[i].state == 3) continue;             /* skip dead */
        p = sapp(b, p, max, "  ");  p = sdec(b, p, max, (uint64_t)ti[i].id);
        p = sapp(b, p, max, "    "); p = sapp(b, p, max, st[(unsigned)ti[i].state < 5 ? ti[i].state : 0]);
        p = sapp(b, p, max, "  ");  p = sdec(b, p, max, ti[i].run_ms);
        p = sapp(b, p, max, "    "); p = sdec(b, p, max, (ti[i].run_ms * 100) / up); p = sapp(b, p, max, "%");
        p = sapp(b, p, max, "    "); p = sdec(b, p, max, ti[i].nswitch);
        p = sapp(b, p, max, "  ");  p = sapp(b, p, max, ti[i].proc ? app_title((app_t *)ti[i].proc) : "(kernel)");
        p = sapp(b, p, max, "\n");
    }
    b[p] = 0; return p;
}

/* ---- the directory tables ------------------------------------------------- */
struct pf { const char *name; long (*gen)(char *, int); };
static const struct pf proc_files[] = {
    { "meminfo", gen_meminfo }, { "uptime", gen_uptime }, { "cpuinfo", gen_cpuinfo },
    { "version", gen_version }, { "loadavg", gen_loadavg }, { "stat", gen_stat },
    { "processes", gen_processes }, { "partitions", gen_partitions },
    { "filesystems", gen_filesystems }, { "mounts", gen_mounts },
    { "interrupts", gen_interrupts }, { "kmsg", gen_kmsg }, { "sched", gen_sched },
    { "kallsyms", gen_kallsyms }, { "net", gen_net }, { "fsevents", gen_fsevents },
    { "profile", gen_profile }, { "ipc", gen_ipc }, { "binds", gen_binds },
    { "bcache", gen_bcache }, { "measure", gen_measure }, { "cas", gen_cas }, { "fw", gen_fw },
    { "notify", gen_notify }, { "swaps", gen_swaps }, { "shm", gen_shm }, { "events", gen_events },
};
static const char *dev_files[] = { "null", "zero", "random", "urandom", "full", "clipboard" };
#define NPROC (int)(sizeof(proc_files)/sizeof(proc_files[0]))
#define NDEV  (int)(sizeof(dev_files)/sizeof(dev_files[0]))

int procfs_is_dir(const char *abs) {
    return peq(abs, "/proc") || peq(abs, "/proc/") || peq(abs, "/dev") || peq(abs, "/dev/");
}
int procfs_owns(const char *abs) {
    return startswith(abs, "/proc/") || startswith(abs, "/dev/") || procfs_is_dir(abs);
}

/* --- per-process /proc/<pid>/{status,cmdline,ctl} (Plan 9 / Linux style) --- */
static int proc_pid_path(const char *abs, int *pid, const char **file) {
    if (!startswith(abs, "/proc/")) return 0;
    const char *p = abs + 6;
    if (startswith(p, "self/")) {                 /* /proc/self/... -> the calling task */
        *pid = task_current_id(); *file = p + 5;
        return **file != 0;
    }
    if (*p < '1' || *p > '9') return 0;          /* a pid starts 1-9; flat files start with a letter */
    int n = 0; while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    if (*p != '/' || p[1] == 0) return 0;        /* must be "/proc/<pid>/<file>" */
    *pid = n; *file = p + 1;
    return 1;
}
static void *proc_find(int pid, int *state_out) {
    task_info_t ti[24]; int cnt = task_snapshot(ti, 24);
    for (int i = 0; i < cnt; i++)
        if (ti[i].id == pid && ti[i].proc) { if (state_out) *state_out = ti[i].state; return ti[i].proc; }
    return 0;
}
static long gen_pid_status(char *b, int max, int pid, int state, void *proc) {
    static const char *st[5] = { "ready", "running", "blocked", "dead", "stopped" };
    int p = 0;
    p = sapp(b, p, max, "Name:\t");        p = sapp(b, p, max, app_title((app_t *)proc)); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "Pid:\t");         p = sdec(b, p, max, (uint64_t)pid);            p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "State:\t");       p = sapp(b, p, max, st[(state >= 0 && state < 5) ? state : 0]); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "HeapKB:\t");      p = sdec(b, p, max, app_heap_bytes((app_t *)proc) / 1024); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "MmapRegions:\t"); p = sdec(b, p, max, (uint64_t)app_vma_count((app_t *)proc)); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "Pledge:\t");
    if (app_is_pledged((app_t *)proc)) {
        char pl[64]; app_pledge_format(app_promises((app_t *)proc), pl, sizeof pl);
        p = sapp(b, p, max, pl[0] ? pl : "(none)");
    } else p = sapp(b, p, max, "(unrestricted)");
    p = sapp(b, p, max, "\n");
    b[p] = 0; return p;
}
/* /proc/<pid>/wss: working-set size from the CPU's Accessed/Dirty PTE bits.
 * "Referenced" counts pages touched since the last `clearref` (write it to ctl
 * to reset the window) — the building block for an LRU/swap victim picker. */
static long gen_pid_wss(char *b, int max, int pid, void *proc) {
    vmm_wss_t w; vmm_wss(app_cr3((app_t *)proc), &w);
    int p = 0;
    p = sapp(b, p, max, "Pid:\t");          p = sdec(b, p, max, (uint64_t)pid);       p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "Resident:\t");     p = sdec(b, p, max, w.resident);          p = sapp(b, p, max, " pages\n");
    p = sapp(b, p, max, "ResidentKB:\t");   p = sdec(b, p, max, w.resident * 4);      p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "Referenced:\t");   p = sdec(b, p, max, w.referenced);        p = sapp(b, p, max, " pages (accessed since clearref)\n");
    p = sapp(b, p, max, "Dirty:\t");        p = sdec(b, p, max, w.dirty);             p = sapp(b, p, max, " pages (written)\n");
    p = sapp(b, p, max, "Writable:\t");     p = sdec(b, p, max, w.writable);          p = sapp(b, p, max, " pages\n");
    b[p] = 0; return p;
}

/* /proc/<pid>/mem/<hexaddr>[/<len>]: hexdump another process's memory — the live
 * counterpart to the post-mortem core reader (crashinfo, M1112). The name-based
 * VFS has no fd offsets, so the address+length ride in the path. Restricted to
 * the user footprint [0x40000000,0x80001000) so it can never expose the shared
 * kernel higher-half or the low identity map; bytes outside a mapped page print
 * as "..". M1114. */
#define MEM_USER_LO 0x40000000ull
#define MEM_USER_HI 0x80001000ull
static int hx(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int shexw(char *b, int p, int max, uint64_t v, int width) {
    for (int i = width - 1; i >= 0; i--) { int nyb = (int)((v >> (i * 4)) & 0xF); if (p < max - 1) b[p++] = (char)(nyb < 10 ? '0' + nyb : 'a' + nyb - 10); }
    return p;
}
static long gen_pid_mem(char *b, int max, void *proc, const char *spec) {
    uint64_t va = 0; const char *s = spec; int v;
    while ((v = hx(*s)) >= 0) { va = (va << 4) | (uint64_t)v; s++; }
    unsigned long len = 64;
    if (*s == '/') { s++; len = 0; while (*s >= '0' && *s <= '9') { len = len * 10 + (unsigned long)(*s - '0'); s++; } }
    if (len == 0) len = 64;
    if (len > 256) len = 256;                              /* one screenful per read */
    uint64_t cr3 = app_cr3((app_t *)proc);
    int p = 0;
    for (unsigned long i = 0; i < len; i += 16) {
        p = sapp(b, p, max, "  "); p = shexw(b, p, max, va + i, 8); p = sapp(b, p, max, ": ");
        char ascii[17]; int na = 0;
        for (int j = 0; j < 16; j++) {
            if (i + (unsigned long)j >= len) { p = sapp(b, p, max, "   "); continue; }
            uint64_t cur = va + i + (unsigned long)j;
            uint64_t phys = (cur >= MEM_USER_LO && cur < MEM_USER_HI) ? vmm_translate_in(cr3, cur) : 0;
            if (phys) {
                unsigned char by = *((volatile unsigned char *)hhdm(phys));
                p = shexw(b, p, max, by, 2); p = sapp(b, p, max, " ");
                ascii[na++] = (by >= 32 && by < 127) ? (char)by : '.';
            } else {
                p = sapp(b, p, max, ".. "); ascii[na++] = '.';
            }
        }
        ascii[na] = 0;
        p = sapp(b, p, max, " |"); p = sapp(b, p, max, ascii); p = sapp(b, p, max, "|\n");
    }
    b[p] = 0; return p;
}

/* /proc/<pid>/regs: the ring-3 register file captured at the target's most recent
 * trap (M1119). Most meaningful for a STOPPED task (its frame is frozen); for a
 * running task it's the last trap (constantly changing). Pairs with /proc/<pid>/mem
 * + ctl stop/cont to inspect a halted process. */
static int rreg(char *b, int p, int max, const char *nm, uint64_t v) {
    p = sapp(b, p, max, nm); p = sapp(b, p, max, "="); p = shexw(b, p, max, v, 16); return p;
}
static long gen_pid_regs(char *b, int max, void *proc) {
    struct registers *r = task_uframe((task_t *)app_task((app_t *)proc));
    if (!r) { int p = sapp(b, 0, max, "  (no saved trap frame — the task has not entered ring 3)\n"); if (p < max) b[p] = 0; return p; }
    int p = 0;
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rip", r->rip); p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rsp", r->rsp);    p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rbp", r->rbp); p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rfl", r->rflags); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rax", r->rax); p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rbx", r->rbx); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rcx", r->rcx); p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rdx", r->rdx); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rsi", r->rsi); p = sapp(b, p, max, "  "); p = rreg(b, p, max, "rdi", r->rdi); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "r8 ", r->r8);  p = sapp(b, p, max, "  "); p = rreg(b, p, max, "r9 ", r->r9);  p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "r10", r->r10); p = sapp(b, p, max, "  "); p = rreg(b, p, max, "r11", r->r11); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "r12", r->r12); p = sapp(b, p, max, "  "); p = rreg(b, p, max, "r13", r->r13); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "r14", r->r14); p = sapp(b, p, max, "  "); p = rreg(b, p, max, "r15", r->r15); p = sapp(b, p, max, "\n");
    p = sapp(b, p, max, "  "); p = rreg(b, p, max, "cs",  r->cs);  p = sapp(b, p, max, "  "); p = rreg(b, p, max, "ss",  r->ss);  p = sapp(b, p, max, "\n");
    if (p < max) b[p] = 0; return p;
}

/* /proc/<pid>/sstrace: the hardware single-step instruction trace (M1123) — the
 * RIPs recorded by the last sys_singlestep(n), oldest-first. */
static long gen_pid_sstrace(char *b, int max, void *proc) {
    uint64_t rips[64]; int n = app_sstep_get((app_t *)proc, rips, 64);
    int p = sapp(b, 0, max, "  #  INSTRUCTION POINTER\n");
    for (int i = 0; i < n; i++) {
        p = sapp(b, p, max, "  "); p = sdec(b, p, max, (uint64_t)i); p = sapp(b, p, max, "  0x");
        p = shexw(b, p, max, rips[i], 8); p = sapp(b, p, max, "\n");
    }
    if (!n) p = sapp(b, p, max, "  (none — sys_singlestep(n) first)\n");
    if (p < max) b[p] = 0; return p;
}

long procfs_read(const char *abs, void *buf, unsigned long max) {
    if (max == 0) return -1;
    if (startswith(abs, "/proc/")) {
        int pid; const char *file;
        if (proc_pid_path(abs, &pid, &file)) {            /* /proc/<pid>/... */
            int st = 0; void *proc = proc_find(pid, &st);
            if (!proc) return -1;
            if (peq(file, "status"))  return gen_pid_status((char *)buf, (int)max, pid, st, proc);
            if (peq(file, "wss"))     return gen_pid_wss((char *)buf, (int)max, pid, proc);
            if (startswith(file, "mem/")) return gen_pid_mem((char *)buf, (int)max, proc, file + 4);
            if (peq(file, "strace")) return strace_format(pid, (char *)buf, (int)max);   /* traced syscalls (M1118) */
            if (peq(file, "regs"))   return gen_pid_regs((char *)buf, (int)max, proc);   /* ring-3 register file (M1119) */
            if (peq(file, "sstrace")) return gen_pid_sstrace((char *)buf, (int)max, proc); /* single-step trace (M1123) */
            if (peq(file, "maps"))    return app_format_maps((app_t *)proc, (char *)buf, (int)max);
            if (peq(file, "cmdline")) {
                char *bb = (char *)buf; int p = sapp(bb, 0, (int)max, app_title((app_t *)proc));
                const char *arg = app_arg((app_t *)proc);
                if (arg && arg[0]) { p = sapp(bb, p, (int)max, " "); p = sapp(bb, p, (int)max, arg); }
                p = sapp(bb, p, (int)max, "\n"); bb[p] = 0; return p;
            }
            return -1;                                    /* ctl is write-only; unknown file */
        }
        const char *f = abs + 6;
        for (int i = 0; i < NPROC; i++)
            if (peq(f, proc_files[i].name)) return proc_files[i].gen((char *)buf, (int)max);
        return -1;
    }
    if (startswith(abs, "/dev/")) {
        const char *f = abs + 5;
        if (peq(f, "null"))   return 0;                         /* EOF */
        if (peq(f, "zero") || peq(f, "full")) {                 /* a bufferful of zeros */
            unsigned long n = max < 4096 ? max : 4096;
            for (unsigned long i = 0; i < n; i++) ((char *)buf)[i] = 0;
            return (long)n;
        }
        if (peq(f, "random") || peq(f, "urandom")) {            /* CSPRNG bytes (hardware-seeded) */
            unsigned long n = max < 4096 ? max : 4096;
            random_bytes(buf, n);
            return (long)n;
        }
        if (peq(f, "clipboard"))                                 /* the system clipboard as a file */
            return (long)clip_get((char *)buf, (int)max);
    }
    return -1;
}

long procfs_write(const char *abs, const void *buf, unsigned long len) {
    if (startswith(abs, "/dev/")) {
        const char *f = abs + 5;
        if (peq(f, "null") || peq(f, "zero")) return (long)len;  /* discard, "succeed" */
        if (peq(f, "full")) return -1;                           /* always ENOSPC */
        if (peq(f, "clipboard")) { clip_set((const char *)buf, (int)len); return (long)len; }
        return -1;                                               /* other /dev nodes: read-only */
    }
    if (startswith(abs, "/proc/")) {
        if (peq(abs + 6, "profile")) {                               /* echo on|off|reset > /proc/profile (M1086) */
            char cmd[16]; int c = 0; const char *s = (const char *)buf;
            for (unsigned long i = 0; i < len && c < 15 && s[i] && s[i] != '\n' && s[i] != ' '; i++) cmd[c++] = s[i];
            cmd[c] = 0; prof_control(cmd); return (long)len;
        }
        if (peq(abs + 6, "fw")) {                                    /* echo "drop in icmp" > /proc/fw (M1100) */
            char cmd[64]; int c = 0; const char *s = (const char *)buf;
            for (unsigned long i = 0; i < len && c < 63 && s[i] && s[i] != '\n'; i++) cmd[c++] = s[i];
            cmd[c] = 0; fw_control(cmd, c); return (long)len;
        }
        int pid; const char *file;
        if (proc_pid_path(abs, &pid, &file) && peq(file, "ctl")) {   /* echo CMD > /proc/<pid>/ctl */
            void *proc = proc_find(pid, 0);
            if (!proc) return -1;
            char cmd[16]; int c = 0; const char *s = (const char *)buf;
            for (unsigned long i = 0; i < len && c < 15 && s[i] && s[i] != '\n' && s[i] != ' '; i++) cmd[c++] = s[i];
            cmd[c] = 0;
            if (peq(cmd, "kill")) { app_request_kill((app_t *)proc); return (long)len; }
            if (peq(cmd, "stop")) { task_stop((task_t *)app_task((app_t *)proc)); return (long)len; }
            if (peq(cmd, "cont")) { task_cont((task_t *)app_task((app_t *)proc)); return (long)len; }
            if (peq(cmd, "trace"))   { app_set_traced((app_t *)proc, 1); return (long)len; }   /* strace -> dmesg (M1084) */
            if (peq(cmd, "untrace")) { app_set_traced((app_t *)proc, 0); return (long)len; }
            if (peq(cmd, "clearref")) { vmm_clear_accessed(app_cr3((app_t *)proc)); return (long)len; }  /* reset the /proc/<pid>/wss window (M1093) */
            return -1;                                            /* unknown command */
        }
        return -1;                                               /* /proc otherwise read-only */
    }
    return -2;                                                   /* not ours */
}

int procfs_list(const char *dir, vfs_dirent *out, int max) {
    int n = 0;
    if (peq(dir, "/proc") || peq(dir, "/proc/")) {
        for (int i = 0; i < NPROC && n < max; i++) {
            int k = 0; const char *s = proc_files[i].name;
            while (s[k] && k < 62) { out[n].name[k] = s[k]; k++; }
            out[n].name[k] = 0; out[n].size = 0; out[n].date = out[n].time = 0; n++;
        }
    } else if (peq(dir, "/dev") || peq(dir, "/dev/")) {
        for (int i = 0; i < NDEV && n < max; i++) {
            int k = 0; const char *s = dev_files[i];
            while (s[k] && k < 62) { out[n].name[k] = s[k]; k++; }
            out[n].name[k] = 0; out[n].size = 0; out[n].date = out[n].time = 0; n++;
        }
    }
    return n;
}
