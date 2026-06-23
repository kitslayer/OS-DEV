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
#include "timer.h"
#include "task.h"
#include "app.h"
#include "blockdev.h"
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
static long gen_stat(char *b, int max) {
    int p = sapp(b, 0, max, "processes ");
    p = sdec(b, p, max, (uint64_t)task_count());
    p = sapp(b, p, max, "\nbtime 0\nuptime_ms ");
    p = sdec(b, p, max, timer_ms());
    p = sapp(b, p, max, "\n");
    b[p] = 0; return p;
}

/* ---- /dev character devices ---------------------------------------------- */
static uint64_t rng_state;
static uint64_t rng_next(void) {
    if (!rng_state) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); rng_state = ((uint64_t)hi << 32 | lo) | 1; }
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state ^ timer_ms();
}

/* ---- the directory tables ------------------------------------------------- */
struct pf { const char *name; long (*gen)(char *, int); };
static const struct pf proc_files[] = {
    { "meminfo", gen_meminfo }, { "uptime", gen_uptime }, { "cpuinfo", gen_cpuinfo },
    { "version", gen_version }, { "loadavg", gen_loadavg }, { "stat", gen_stat },
    { "processes", gen_processes }, { "partitions", gen_partitions },
    { "filesystems", gen_filesystems }, { "mounts", gen_mounts },
};
static const char *dev_files[] = { "null", "zero", "random", "full" };
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
    b[p] = 0; return p;
}

long procfs_read(const char *abs, void *buf, unsigned long max) {
    if (max == 0) return -1;
    if (startswith(abs, "/proc/")) {
        int pid; const char *file;
        if (proc_pid_path(abs, &pid, &file)) {            /* /proc/<pid>/... */
            int st = 0; void *proc = proc_find(pid, &st);
            if (!proc) return -1;
            if (peq(file, "status"))  return gen_pid_status((char *)buf, (int)max, pid, st, proc);
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
        if (peq(f, "random")) {                                 /* pseudo-random bytes */
            unsigned long n = max < 4096 ? max : 4096;
            for (unsigned long i = 0; i < n; i++) ((unsigned char *)buf)[i] = (unsigned char)(rng_next() >> 11);
            return (long)n;
        }
    }
    return -1;
}

long procfs_write(const char *abs, const void *buf, unsigned long len) {
    if (startswith(abs, "/dev/")) {
        const char *f = abs + 5;
        if (peq(f, "null") || peq(f, "zero")) return (long)len;  /* discard, "succeed" */
        if (peq(f, "full")) return -1;                           /* always ENOSPC */
        return -1;                                               /* other /dev nodes: read-only */
    }
    if (startswith(abs, "/proc/")) {
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
