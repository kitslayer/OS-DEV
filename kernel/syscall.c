/*
 * syscall.c — the kernel side of the system-call interface.
 *
 * Reached via `int 0x80` from ring 3. The interrupt stub saved the user's
 * registers into `struct registers`; we read the call number from rax and the
 * arguments from rdi/rsi/rdx, do the work, and store the result back in rax
 * (which the stub restores into the user's rax on the way out).
 */
#define __KERNEL__
#include "syscall.h"
#include "interrupts.h"
#include "console.h"
#include "app.h"
#include "strace.h"
#include "vfs.h"
#include "fb.h"
#include "rtc.h"
#include "speaker.h"
#include "pmm.h"
#include "vmm.h"
#include "timer.h"
#include "task.h"
#include "io.h"
#include "net.h"
#include "tls.h"
#include "js.h"
#include "sha256.h"
#include "sha512.h"
#include "aes.h"
#include "string.h"
#include "kheap.h"
#include "inflate.h"
#include "zip.h"
#include "tar.h"
#include "audio.h"
#include "desktop.h"
#include "pci.h"
#include "blockdev.h"
#include "random.h"
#include "cas.h"
#include "acpi.h"
#include <stdint.h>

/* Validate a user-supplied syscall pointer argument: the range [p, p+n) must
 * lie entirely within the calling app's own user (PTE_USER) pages. A syscall
 * runs in ring 0 with the app's CR3 active, where kernel memory is mapped and
 * writable — so without this an app could hand a kernel pointer to a handler
 * and have the kernel read or (worse) write its own memory. On failure the
 * handler returns -1 instead of touching the bogus address. `n` is the exact
 * number of bytes the handler will access through the pointer. */
static int ubuf(uint64_t p, uint64_t n) { return vmm_user_ok(p, n); }

/* Validate a user-supplied NUL-terminated string argument (filename, hostname,
 * URL, script): every byte up to the terminator must be in the app's own user
 * pages. Bounds the scan generously (16 MiB) — longer than any real arg, but
 * larger than a page-table walk needs to reject a non-terminated/forged string.
 * A handler that reads a string from the arg checks this before dereferencing. */
static int ustr(uint64_t p) { return vmm_user_str_ok(p, 16u << 20); }

/* SYS_unzip helper: extract callback. Mangles each archived path to an 8.3 name
 * (basename, upper-cased, <=8 chars + '.' + <=3-char ext) and writes it via the
 * VFS, counting successes in ctx. */
struct unzip_ctx { int written; };
static void unzip_emit(void *vctx, const char *name, int namelen,
                       const uint8_t *data, int datalen) {
    struct unzip_ctx *c = (struct unzip_ctx *)vctx;
    int base = 0;
    for (int i = 0; i < namelen; i++) if (name[i] == '/') base = i + 1;   /* drop directories */
    int dot = -1;
    for (int i = base; i < namelen; i++) if (name[i] == '.') dot = i;
    int nend = (dot >= 0) ? dot : namelen;
    char fn[13]; int p = 0;
    for (int i = base; i < nend && p < 8; i++) {
        char ch = name[i]; if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        fn[p++] = ch;
    }
    if (dot >= 0) {
        fn[p++] = '.';
        for (int i = dot + 1; i < namelen && p < 12; i++) {
            char ch = name[i]; if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            fn[p++] = ch;
        }
    }
    fn[p] = 0;
    if (p == 0 || (p == 1 && fn[0] == '.')) return;          /* nothing usable */
    if (vfs_write(fn, data, (unsigned long)datalen) >= 0) c->written++;
}

static void put2(char *p, int v) { p[0] = '0' + (v / 10) % 10; p[1] = '0' + v % 10; }

static int sappend(char *d, int n, int max, const char *s) {
    while (*s && n + 1 < max) d[n++] = *s++;     /* n+1<max: safe even if max<=0 */
    return n;
}
static int snum(char *d, int n, int max, uint64_t v) {
    char t[21]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = '0' + v % 10; v /= 10; }
    while (i && n + 1 < max) d[n++] = t[--i];
    return n;
}

/* Minimal unsigned-int-to-decimal for the directory listing. */
static int u32_to_dec(uint32_t v, char *out) {
    char tmp[12];
    int i = 0, n = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = '0' + v % 10; v /= 10; }
    while (i) out[n++] = tmp[--i];
    return n;
}

/* Read an entire file into a freshly kmalloc'd buffer. The read API has no size
 * query, so grow the buffer (re-reading) until the read no longer fills it —
 * the whole file is returned, not a fixed-size prefix. Returns the length (and
 * sets *out, which the caller kfree's), or -1 on missing file / >=32MB / OOM. */
static long read_whole_file(const char *name, uint8_t **out) {
    size_t cap = 65536;
    uint8_t *buf = kmalloc(cap);
    long n = buf ? vfs_read(name, buf, cap) : -1;
    while (buf && n == (long)cap && cap < (32u << 20)) {   /* filled the buffer: file may be larger */
        cap <<= 1; kfree(buf); buf = kmalloc(cap);
        if (buf) n = vfs_read(name, buf, cap);
    }
    if (!buf) return -1;
    if (n < 0 || n == (long)cap) { kfree(buf); return -1; }   /* read error, or file >= 32MB */
    *out = buf;
    return n;
}

/* Append two lowercase hex digits of `v` (0..255). Used by the lspci formatter
 * for the bus/slot/func and vendor:device:class fields. */
static int shex2(char *d, int n, int max, uint8_t v) {
    static const char hx[] = "0123456789abcdef";
    if (n + 1 < max) d[n++] = hx[(v >> 4) & 0xF];
    if (n + 1 < max) d[n++] = hx[v & 0xF];
    return n;
}
static int shex4(char *d, int n, int max, uint16_t v) {
    n = shex2(d, n, max, (uint8_t)(v >> 8));
    n = shex2(d, n, max, (uint8_t)v);
    return n;
}

/* Human-readable name for one PCI device, for the lspci listing. We pick a name
 * by class (more useful than the vendor for "what is this"), refining a few
 * subclasses; unknown classes fall back to the vendor name, then to "device".
 * Just the common QEMU/PC hardware — anything unrecognized still shows as hex. */
static const char *pci_class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
    case 0x00: return "unclassified";
    case 0x01:                                  /* mass storage */
        switch (sub) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller (AHCI)";
        case 0x08: return "NVMe controller";
        default:   return "storage controller";
        }
    case 0x02: return "Ethernet controller";
    case 0x03: return "VGA display";
    case 0x04:                                  /* multimedia */
        return (sub == 0x01) ? "audio controller" : "multimedia controller";
    case 0x05: return "memory controller";
    case 0x06:                                  /* bridge */
        switch (sub) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        case 0x80: return "bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x0C:                                  /* serial bus */
        switch (sub) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "serial bus controller";
        }
    default:   return 0;                        /* let the caller try the vendor */
    }
}
static const char *pci_vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x10EC: return "Realtek";
    case 0x1AF4: return "Red Hat / virtio";
    case 0x1B36: return "Red Hat";
    case 0x1234: return "QEMU / Bochs";
    case 0x80EE: return "VirtualBox";
    case 0x1022: return "AMD";
    case 0x106B: return "Apple";
    default:     return 0;
    }
}

/* Format the full PCI device list into [buf,max) as lines of the form
 *   "BB:SS.F vendor:device class CC:SS  <name>\n"
 * one per device. Returns the byte count written (NUL-terminated if room).
 * Caller has already validated [buf,max) lies in the app's own pages. */
static int pci_format(char *b, int max) {
    pci_device_t devs[64];
    int total = pci_collect(devs, 64);
    int cnt = (total < 64) ? total : 64;            /* only `cnt` were stored */
    int p = 0;
    for (int i = 0; i < cnt; i++) {
        pci_device_t *d = &devs[i];
        p = shex2(b, p, max, d->bus);
        p = sappend(b, p, max, ":");
        p = shex2(b, p, max, d->slot);
        p = sappend(b, p, max, ".");
        if (p + 1 < max) b[p++] = (char)('0' + (d->func & 7));
        p = sappend(b, p, max, " ");
        p = shex4(b, p, max, d->vendor_id);
        p = sappend(b, p, max, ":");
        p = shex4(b, p, max, d->device_id);
        p = sappend(b, p, max, " class ");
        p = shex2(b, p, max, d->class_id);
        p = sappend(b, p, max, ":");
        p = shex2(b, p, max, d->subclass);
        p = sappend(b, p, max, "  ");
        const char *name = pci_class_name(d->class_id, d->subclass);
        if (!name) name = pci_vendor_name(d->vendor_id);
        if (!name) name = "device";
        p = sappend(b, p, max, name);
        /* tack the vendor on for recognized-class devices, so e.g. an Ethernet
         * line reads "Ethernet controller (Intel)" — handy for picking drivers */
        const char *vn = pci_vendor_name(d->vendor_id);
        if (vn && pci_class_name(d->class_id, d->subclass)) {
            p = sappend(b, p, max, " (");
            p = sappend(b, p, max, vn);
            p = sappend(b, p, max, ")");
        }
        p = sappend(b, p, max, "\n");
    }
    if (cnt == 0) p = sappend(b, p, max, "  (no PCI devices)\n");
    if (p < max) b[p] = 0;
    return p;
}

/* Which pledge() promise class a syscall needs (0 = always allowed even when
 * pledged: exit, sigreturn, getpid, and pledge itself). See app.h for the bits. */
static uint32_t syscall_class(uint64_t nr) {
    switch (nr) {
    case SYS_exit: case SYS_sigreturn: case SYS_getpid: case SYS_pledge: return 0;
    case SYS_write: case SYS_read: case SYS_time: case SYS_sysinfo: case SYS_clear:
    case SYS_pollkey: case SYS_sleep: case SYS_uptime_ms: case SYS_sbrk: case SYS_getarg:
    case SYS_history: case SYS_setcolor: case SYS_caret: case SYS_signal: case SYS_raise:
    case SYS_alarm:
    case SYS_getrandom: case SYS_setkbmode: case SYS_getkbevent: case SYS_mouse:
    case SYS_mouse_rel: case SYS_beep:
        return PL_STDIO;
    case SYS_readfile: case SYS_list: case SYS_tree: case SYS_df: case SYS_find:
    case SYS_chdir: case SYS_lsblk: case SYS_lspci: case SYS_mounts:
    case SYS_sha256: case SYS_sha512: case SYS_cas_fetch: case SYS_losetup:
        return PL_RPATH;
    case SYS_writefile: case SYS_delete: case SYS_mkdir: case SYS_crypt:
    case SYS_gzip: case SYS_gunzip: case SYS_unzip: case SYS_untar:
    case SYS_savebmp: case SYS_screenshot: case SYS_setwall: case SYS_cas_store:
        return PL_WPATH;
    case SYS_ping: case SYS_resolve: case SYS_http: case SYS_https: case SYS_browse:
    case SYS_pinghost: case SYS_netinfo: case SYS_dhcp: case SYS_tftp: case SYS_sntp:
        return PL_INET;
    case SYS_gfx_init: case SYS_gfx_blit: case SYS_pcm: case SYS_playwav:
    case SYS_pcm_stream: case SYS_pcm_avail: case SYS_playbg: case SYS_audiostop:
    case SYS_clip_get: case SYS_clip_set:
        return PL_GFX;
    case SYS_spawn: case SYS_fork: case SYS_waitpid: case SYS_exec: case SYS_kill: case SYS_ps: case SYS_apps: case SYS_js:
        return PL_PROC;
    case SYS_mmap: case SYS_munmap: case SYS_madvise: case SYS_swapout: case SYS_shm_open: case SYS_futex:
        return PL_VM;
    case SYS_poweroff: case SYS_reboot:
        return PL_POWER;
    default:
        return 0;        /* unmapped -> always allowed (never brick on an unknown call) */
    }
}

/* Human-readable syscall names for the strace channel (M1084). Designated
 * initialisers keep it in sync with syscall.h by number. */
static const char *syscall_name(uint64_t n) {
    static const char *const nm[] = {
        [SYS_write]="write",[SYS_exit]="exit",[SYS_getpid]="getpid",[SYS_read]="read",
        [SYS_list]="list",[SYS_readfile]="readfile",[SYS_time]="time",[SYS_beep]="beep",
        [SYS_sysinfo]="sysinfo",[SYS_clear]="clear",[SYS_reboot]="reboot",[SYS_writefile]="writefile",
        [SYS_ping]="ping",[SYS_resolve]="resolve",[SYS_delete]="delete",[SYS_spawn]="spawn",
        [SYS_sleep]="sleep",[SYS_http]="http",[SYS_browse]="browse",[SYS_mkdir]="mkdir",
        [SYS_chdir]="chdir",[SYS_tree]="tree",[SYS_ps]="ps",[SYS_pollkey]="pollkey",
        [SYS_df]="df",[SYS_find]="find",[SYS_sha256]="sha256",[SYS_crypt]="crypt",
        [SYS_history]="history",[SYS_https]="https",[SYS_js]="js",[SYS_setcolor]="setcolor",
        [SYS_pinghost]="pinghost",[SYS_netinfo]="netinfo",[SYS_apps]="apps",[SYS_sha512]="sha512",
        [SYS_screenshot]="screenshot",[SYS_gunzip]="gunzip",[SYS_gzip]="gzip",[SYS_unzip]="unzip",
        [SYS_untar]="untar",[SYS_sbrk]="sbrk",[SYS_uptime_ms]="uptime_ms",[SYS_gfx_init]="gfx_init",
        [SYS_gfx_blit]="gfx_blit",[SYS_setkbmode]="setkbmode",[SYS_getkbevent]="getkbevent",[SYS_pcm]="pcm",
        [SYS_playwav]="playwav",[SYS_pcm_stream]="pcm_stream",[SYS_pcm_avail]="pcm_avail",[SYS_mouse]="mouse",
        [SYS_playbg]="playbg",[SYS_audiostop]="audiostop",[SYS_mouse_rel]="mouse_rel",[SYS_caret]="caret",
        [SYS_clip_get]="clip_get",[SYS_clip_set]="clip_set",[SYS_getarg]="getarg",[SYS_savebmp]="savebmp",
        [SYS_setwall]="setwall",[SYS_lspci]="lspci",[SYS_lsblk]="lsblk",[SYS_poweroff]="poweroff",
        [SYS_kill]="kill",[SYS_mounts]="mounts",[SYS_mmap]="mmap",[SYS_munmap]="munmap",
        [SYS_signal]="signal",[SYS_raise]="raise",[SYS_sigreturn]="sigreturn",[SYS_getrandom]="getrandom",
        [SYS_pledge]="pledge",[SYS_unveil]="unveil",[SYS_symlink]="symlink",
        [SYS_jail]="jail",[SYS_ringbuf]="ringbuf",[SYS_mprotect]="mprotect",[SYS_bind]="bind",
        [SYS_dhcp]="dhcp",[SYS_cas_store]="cas_store",[SYS_cas_fetch]="cas_fetch",
        [SYS_tftp]="tftp",[SYS_madvise]="madvise",[SYS_alarm]="alarm",[SYS_sntp]="sntp",
        [SYS_swapout]="swapout",[SYS_losetup]="losetup",[SYS_shm_open]="shm_open",[SYS_futex]="futex",
        [SYS_fork]="fork",[SYS_waitpid]="waitpid",[SYS_exec]="exec",[SYS_unshare]="unshare",
        [SYS_singlestep]="singlestep",
        [SYS_seccomp]="seccomp",[SYS_seccomp_wait]="seccomp_wait",[SYS_seccomp_reply]="seccomp_reply",
        [SYS_fswait]="fswait",[SYS_signalfd]="signalfd",
    };
    return (n < sizeof nm / sizeof nm[0] && nm[n]) ? nm[n] : "?";
}

void syscall_dispatch(struct registers *r) {
    app_t *self = app_current();

    /* strace (M1084): snapshot the call BEFORE the switch (args, in case a handler
     * reuses the register slots); emitted after, with the result, if traced. */
    int traced = app_is_traced(self);
    uint64_t tr_nr = r->rax, tr_a = r->rdi, tr_b = r->rsi, tr_c = r->rdx;

    /* pledge() enforcement: a pledged app that calls a syscall outside its kept
     * classes is killed on the spot (like OpenBSD's SIGABRT). Unpledged apps —
     * every existing program — are unaffected. */
    if (self && app_is_pledged(self)) {
        uint32_t need = syscall_class(r->rax);
        if (need && !(app_promises(self) & need)) {
            kprintf("[pledge] pid %d (%s) called a syscall outside its pledge (sys %lu) -- killing\n",
                    app_sys_getpid(), app_title(self), (unsigned long)r->rax);
            app_sys_exit(-1);               /* terminate the violating app; does not return */
        }
    }

    /* seccomp-notify (M1124): a supervised process traps masked syscalls to its
     * supervisor, which can deny or emulate them. Unsupervised apps unaffected. */
    if (self && app_seccomp_traps(self, r->rax)) {
        int run_real = 1;
        long ret = app_seccomp_notify(self, r->rax, r->rdi, r->rsi, r->rdx, &run_real);
        if (!run_real) { r->rax = (uint64_t)ret; goto sc_done; }   /* denied/emulated: skip the real syscall */
    }

    switch (r->rax) {
    case SYS_write:
        /* stdout goes to the calling app's window text grid */
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        app_sys_write((const char *)r->rsi, (unsigned)r->rdx);
        r->rax = r->rdx;
        break;
    case SYS_read:
        /* a line of input from the app's window (blocks until Enter) */
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)app_sys_read((char *)r->rsi, (unsigned)r->rdx);
        break;
    case SYS_getpid:
        r->rax = (uint64_t)app_sys_getpid();
        break;
    case SYS_fork:                         /* copy-on-write fork: child gets 0, parent gets the child pid (M1116) */
        r->rax = (uint64_t)app_fork(r);
        break;
    case SYS_waitpid: {                    /* block until a child exits; collect its status (M1117) */
        int *st = (int *)r->rsi;
        if (st && !ubuf(r->rsi, sizeof(int))) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)app_waitpid((int)r->rdi, st);
        break;
    }
    case SYS_exec: {                       /* replace this process's image in place (M1121) */
        const char *nm = (const char *)r->rdi, *ag = (const char *)r->rsi;
        if (!ustr(r->rdi) || (ag && !ustr(r->rsi))) { r->rax = (uint64_t)-1; break; }
        if (app_exec(r, nm, ag) < 0) r->rax = (uint64_t)-1;   /* only reached on failure; success rewrote r */
        break;
    }
    case SYS_list: {
        /* Format the root directory into the user buffer: "name  size\n". */
        char       *buf = (char *)r->rsi;
        uint64_t    max = r->rdx;
        if (max == 0 || !ubuf((uint64_t)buf, max)) { r->rax = (uint64_t)-1; break; }  /* max==0 would underflow `max-1` below */
        static vfs_dirent ents[256];   /* static (not stack): 256*sizeof too big for the 16KB kernel stack */
        int         count = vfs_list(ents, 256);
        uint64_t    n = 0;
        for (int i = 0; i < count; i++) {
            if (n >= max - 1) break;
            for (int j = 0; j < 63 && ents[i].name[j] && n < max - 1; j++)   /* j<63: defensive name bound */
                buf[n++] = ents[i].name[j];
            if (n < max - 1) buf[n++] = ' ';
            char num[12];
            int  ln = u32_to_dec(ents[i].size, num);
            for (int k = 0; k < ln && n < max - 1; k++) buf[n++] = num[k];
            if (ents[i].date && n + 18 < max) {       /* "  YYYY-MM-DD HH:MM" for timestamped files */
                int yr = (ents[i].date >> 9) + 1980, mo = (ents[i].date >> 5) & 15, dy = ents[i].date & 31;
                int hh = (ents[i].time >> 11) & 31,  mi = (ents[i].time >> 5) & 63;
                buf[n++] = ' '; buf[n++] = ' ';
                buf[n++] = '0'+(yr/1000)%10; buf[n++]='0'+(yr/100)%10; buf[n++]='0'+(yr/10)%10; buf[n++]='0'+yr%10;
                buf[n++] = '-'; buf[n++]='0'+(mo/10)%10; buf[n++]='0'+mo%10;
                buf[n++] = '-'; buf[n++]='0'+(dy/10)%10; buf[n++]='0'+dy%10;
                buf[n++] = ' '; buf[n++]='0'+(hh/10)%10; buf[n++]='0'+hh%10;
                buf[n++] = ':'; buf[n++]='0'+(mi/10)%10; buf[n++]='0'+mi%10;
            }
            if (n < max - 1) buf[n++] = '\n';
        }
        buf[n] = '\0';
        r->rax = n;
        break;
    }
    case SYS_readfile: {
        const char *name = (const char *)r->rdi;
        void       *buf  = (void *)r->rsi;
        uint64_t    max  = r->rdx;
        if (!ustr(r->rdi) || !ubuf((uint64_t)buf, max)) { r->rax = (uint64_t)-1; break; }
        if (!app_unveil_ok(self, name, 0)) { r->rax = (uint64_t)-1; break; }   /* hidden by unveil() */
        r->rax = (uint64_t)vfs_read(name, buf, max);
        break;
    }
    case SYS_time: {
        /* write "YYYY-MM-DD HH:MM:SS\n" into the user buffer */
        char *buf = (char *)r->rsi;
        if (r->rdx >= 21) {
            if (!ubuf(r->rsi, 21)) { r->rax = (uint64_t)-1; break; }
            struct rtc_time tm; rtc_now(&tm);
            put2(buf+0, tm.year/100); put2(buf+2, tm.year%100); buf[4]='-';
            put2(buf+5, tm.month); buf[7]='-'; put2(buf+8, tm.day); buf[10]=' ';
            put2(buf+11, tm.hour); buf[13]=':'; put2(buf+14, tm.min);
            buf[16]=':'; put2(buf+17, tm.sec); buf[19]='\n'; buf[20]=0;
            r->rax = 20;
        } else r->rax = 0;
        break;
    }
    case SYS_beep:
        __asm__ volatile("sti");           /* beep() blocks on the timer; need IF=1 */
        beep((uint32_t)r->rdi, (uint32_t)r->rsi);
        break;
    case SYS_sysinfo: {
        char *b = (char *)r->rsi; int max = (int)r->rdx, n = 0;
        if (max <= 0 || !ubuf(r->rsi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }
        n = sappend(b, n, max, "RAM:    ");
        n = snum(b, n, max, pmm_free_bytes() / (1024*1024));
        n = sappend(b, n, max, " MiB free / ");
        n = snum(b, n, max, pmm_total_bytes() / (1024*1024));
        n = sappend(b, n, max, " MiB\nuptime: ");
        n = snum(b, n, max, timer_ticks() / 100);
        n = sappend(b, n, max, " s\ntasks:  ");
        n = snum(b, n, max, (uint64_t)task_count());
        n = sappend(b, n, max, "\n");
        b[n] = 0; r->rax = (uint64_t)n;
        break;
    }
    case SYS_clear:
        app_sys_clear();
        break;
    case SYS_setcolor:
        app_setcolor((int)r->rdi);
        break;
    case SYS_clip_get:
        if (!ubuf(r->rdi, r->rsi)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)clip_get((char *)r->rdi, (int)r->rsi);
        break;
    case SYS_clip_set:
        if (!ubuf(r->rdi, r->rsi)) { r->rax = (uint64_t)-1; break; }
        clip_set((const char *)r->rdi, (int)r->rsi);
        break;
    case SYS_getarg:
        if (!ubuf(r->rdi, r->rsi)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)app_getarg((char *)r->rdi, (int)r->rsi);
        break;
    case SYS_writefile:
        if (!ustr(r->rdi) || !ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        if (!app_unveil_ok(self, (const char *)r->rdi, 1)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)vfs_write((const char *)r->rdi, (const void *)r->rsi, r->rdx);
        break;
    case SYS_delete:
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        if (!app_unveil_ok(self, (const char *)r->rdi, 1)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)vfs_remove((const char *)r->rdi);
        break;
    case SYS_spawn: {
        const char *nm = (const char *)r->rdi;
        const char *arg = (const char *)r->rsi;    /* optional launch arg (e.g. a filename for the editor) */
        if (!ustr(r->rdi) || (arg && !ustr(r->rsi))) { r->rax = (uint64_t)-1; break; }
        int rc = (arg && arg[0]) ? app_spawn_named_arg(nm, arg) : app_spawn_named(nm);  /* a built-in program? */
        if (rc < 0) rc = app_spawn_from_file(nm);  /* else try loading it from disk */
        r->rax = (uint64_t)(int64_t)rc;
        break;
    }
    case SYS_sleep:
        app_kill_check();                  /* WM close-request: a paced game exits instead of sleeping on */
        __asm__ volatile("sti");           /* timer drives the wait */
        timer_wait(r->rdi / 10 + 1);
        break;
    case SYS_ping:
        __asm__ volatile("sti");           /* needs the timer for its timeout */
        r->rax = (uint64_t)(int64_t)net_ping_gateway();
        break;
    case SYS_pinghost:
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        __asm__ volatile("sti");           /* DNS + ICMP both need the timer running */
        r->rax = (uint64_t)(int64_t)net_ping_host((const char *)r->rdi);
        break;
    case SYS_dhcp:
        __asm__ volatile("sti");           /* the DORA handshake needs the timer for its timeouts */
        r->rax = (uint64_t)(int64_t)net_dhcp();
        break;
    case SYS_sntp:
        __asm__ volatile("sti");           /* the query waits for a reply (needs the timer) */
        r->rax = (uint64_t)(int64_t)net_sntp();
        break;
    case SYS_cas_store:                    /* (buf, len, hash32) -> store; write the SHA-256 key */
        if (!ubuf(r->rdi, r->rsi) || !ubuf(r->rdx, 32)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)cas_store((const void *)r->rdi, (uint32_t)r->rsi, (uint8_t *)r->rdx);
        break;
    case SYS_cas_fetch:                    /* (hash32, buf, max) -> fetch by key */
        if (!ubuf(r->rdi, 32) || !ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)cas_fetch((const uint8_t *)r->rdi, (void *)r->rsi, (uint32_t)r->rdx);
        break;
    case SYS_tftp: {                       /* (filename, buf, max) -> TFTP-fetch from the gateway's server */
        if (!ustr(r->rdi) || !ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        const uint8_t *g = net_gateway();  /* SLIRP's TFTP server lives on the gateway */
        char gw[16]; int n = 0;
        for (int o = 0; o < 4; o++) {
            if (o) gw[n++] = '.';
            int v = g[o];
            if (v >= 100) gw[n++] = (char)('0' + v / 100);
            if (v >= 10)  gw[n++] = (char)('0' + (v / 10) % 10);
            gw[n++] = (char)('0' + v % 10);
        }
        gw[n] = 0;
        __asm__ volatile("sti");           /* the lock-step transfer needs the timer for its timeouts */
        r->rax = (uint64_t)(int64_t)net_tftp_get(gw, (const char *)r->rdi, (void *)r->rsi, (uint32_t)r->rdx);
        break;
    }
    case SYS_apps:
        if (!ubuf(r->rdi, r->rsi)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)app_list_names((char *)r->rdi, (int)r->rsi);
        break;
    case SYS_netinfo: {                    /* our IP/MAC/gateway/DNS as aligned text */
        char *b = (char *)r->rdi; int max = (int)r->rsi;
        if (max < 128) { r->rax = (uint64_t)-1; break; }   /* worst case ~96 B; require headroom */
        if (!ubuf(r->rdi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }
        const uint8_t *ip = net_ip(), *gw = net_gateway(), *dns = net_dns(), *m = net_mac();
        static const char H[] = "0123456789abcdef";
        int n = 0;
        const char *labels[4] = { "IP    ", "MAC   ", "GW    ", "DNS   " };
        const uint8_t *v4[4]  = { ip, NULL, gw, dns };     /* slot 1 (MAC) handled specially */
        /* Every raw write is guarded by `n + 1 < max` (leaving room for the
         * trailing NUL), exactly like SYS_resolve above — so the formatter stays
         * memory-safe for ANY buffer size even if fields are added later, rather
         * than relying on the worst-case length staying under the guard. */
        for (int row = 0; row < 4; row++) {
            for (const char *s = labels[row]; *s; s++) if (n + 1 < max) b[n++] = *s;
            if (row == 1) {                                /* MAC: 6 hex bytes, colon-separated */
                for (int i = 0; i < 6; i++) {
                    if (n + 1 < max) b[n++] = H[m[i] >> 4];
                    if (n + 1 < max) b[n++] = H[m[i] & 15];
                    if (i < 5 && n + 1 < max) b[n++] = ':';
                }
            } else {                                       /* IPv4 dotted quad */
                for (int i = 0; i < 4; i++) { n = snum(b, n, max, v4[row][i]); if (i < 3 && n + 1 < max) b[n++] = '.'; }
            }
            if (n + 1 < max) b[n++] = '\n';
        }
        b[n] = 0; r->rax = (uint64_t)n;
        break;
    }
    case SYS_http:
        if (!ustr(r->rdi) || !ustr(r->rsi) || !ubuf(r->rdx, r->r10)) { r->rax = (uint64_t)-1; break; }  /* host, path, response buf */
        __asm__ volatile("sti");           /* TCP needs the timer running */
        r->rax = (uint64_t)(int64_t)http_get((const char *)r->rdi,
                                             (const char *)r->rsi,
                                             (char *)r->rdx, (int)r->r10);
        break;
    case SYS_https:
        if (!ustr(r->rdi) || !ustr(r->rsi) || !ubuf(r->rdx, r->r10)) { r->rax = (uint64_t)-1; break; }  /* host, path, response buf */
        __asm__ volatile("sti");           /* TLS/TCP need the timer running */
        r->rax = (uint64_t)(int64_t)tls_get((const char *)r->rdi,
                                            (const char *)r->rsi,
                                            (uint8_t *)r->rdx, (int)r->r10,
                                            (uint32_t)timer_ticks());
        break;
    case SYS_browse:
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        app_browse((const char *)r->rdi);  /* WM opens the browser window */
        r->rax = 0;
        break;
    case SYS_js:
        if (!ustr(r->rdi) || !ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }  /* script src + result buffer */
        __asm__ volatile("sti");           /* keep the timer live during long scripts */
        r->rax = (uint64_t)(int64_t)js_run((const char *)r->rdi,
                                           (char *)r->rsi, (int)r->rdx);
        break;
    case SYS_mkdir:
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        if (!app_unveil_ok(self, (const char *)r->rdi, 1)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)vfs_mkdir((const char *)r->rdi);
        break;
    case SYS_chdir:
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        if (!app_unveil_ok(self, (const char *)r->rdi, 0)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)vfs_chdir((const char *)r->rdi);
        break;
    case SYS_tree:
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)vfs_tree((char *)r->rsi, (int)r->rdx);
        break;
    case SYS_pollkey:
        r->rax = (uint64_t)(int64_t)app_sys_pollkey();
        break;
    case SYS_find:
        if (!ustr(r->rdi) || !ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }  /* search term + result buffer */
        r->rax = (uint64_t)(int64_t)vfs_find((const char *)r->rdi,
                                             (char *)r->rsi, (int)r->rdx);
        break;
    case SYS_sha256: {
        if ((int)r->rdx < 65) { r->rax = (uint64_t)-1; break; }   /* need room for 64 hex + NUL */
        if (!ustr(r->rdi) || !ubuf(r->rsi, 65)) { r->rax = (uint64_t)-1; break; }  /* filename + hex output */
        uint8_t *fbuf; long fn = read_whole_file((const char *)r->rdi, &fbuf);   /* whole file, not a 16KB prefix */
        if (fn < 0) { r->rax = (uint64_t)-1; break; }
        uint8_t dg[32]; sha256(fbuf, (size_t)fn, dg); kfree(fbuf);
        char *hx = (char *)r->rsi; const char *H = "0123456789abcdef";
        for (int i = 0; i < 32; i++) { hx[i*2] = H[dg[i]>>4]; hx[i*2+1] = H[dg[i]&0xF]; }
        hx[64] = 0; r->rax = 0;
        break;
    }
    case SYS_sha512: {
        if ((int)r->rdx < 129) { r->rax = (uint64_t)-1; break; }   /* need room for 128 hex + NUL */
        if (!ustr(r->rdi) || !ubuf(r->rsi, 129)) { r->rax = (uint64_t)-1; break; }  /* filename + hex output */
        uint8_t *fbuf; long fn = read_whole_file((const char *)r->rdi, &fbuf);   /* whole file, not a 16KB prefix */
        if (fn < 0) { r->rax = (uint64_t)-1; break; }
        uint8_t dg[64]; sha512(fbuf, (size_t)fn, dg); kfree(fbuf);
        char *hx = (char *)r->rsi; const char *H = "0123456789abcdef";
        for (int i = 0; i < 64; i++) { hx[i*2] = H[dg[i]>>4]; hx[i*2+1] = H[dg[i]&0xF]; }
        hx[128] = 0; r->rax = 0;
        break;
    }
    case SYS_screenshot: {
        const char *sn = (const char *)r->rdi;          /* a ".png" name -> PNG, else BMP */
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        int L = 0; while (sn[L]) L++;
        int png = (L >= 4 && sn[L-4]=='.' && (sn[L-3]|32)=='p' && (sn[L-2]|32)=='n' && (sn[L-1]|32)=='g');
        r->rax = (uint64_t)(int64_t)(png ? fb_save_png(sn) : fb_save_bmp(sn));
        break;
    }
    case SYS_gunzip: {
        const char *insrc = (const char *)r->rdi, *outname = (const char *)r->rsi;
        if (!ustr(r->rdi) || !ustr(r->rsi)) { r->rax = (uint64_t)-1; break; }
        uint8_t *in = 0; long gn = read_whole_file(insrc, &in);   /* whole .gz, not a 256KB prefix */
        if (gn < 0) { r->rax = (uint64_t)-1; break; }
        if (gn < 18) { kfree(in); r->rax = (uint64_t)-1; break; }   /* too short to be gzip */
        /* gzip trailer's ISIZE (last 4 bytes) = original size mod 2^32 — size the output to it */
        size_t isize = (size_t)((uint32_t)in[gn-4] | ((uint32_t)in[gn-3]<<8) |
                                ((uint32_t)in[gn-2]<<16) | ((uint32_t)in[gn-1]<<24));
        if (isize == 0) isize = 1;
        if (isize > (32u << 20)) { kfree(in); r->rax = (uint64_t)-1; break; }   /* implausible/too large */
        uint8_t *out = kmalloc(isize);
        long dl = out ? gz_inflate(in, (int)gn, out, (int)isize) : -1;
        if (dl > 0 && vfs_write(outname, out, (unsigned long)dl) < 0) dl = -1;
        if (out) kfree(out);
        kfree(in);                          /* `in` is always allocated here: free unconditionally */
        r->rax = (uint64_t)(int64_t)dl;
        break;
    }
    case SYS_gzip: {
        const char *insrc = (const char *)r->rdi, *outname = (const char *)r->rsi;
        if (!ustr(r->rdi) || !ustr(r->rsi)) { r->rax = (uint64_t)-1; break; }
        uint8_t *in = 0; long gn = read_whole_file(insrc, &in);   /* whole input, not a 256KB prefix */
        if (gn < 0) { r->rax = (uint64_t)-1; break; }
        size_t ocap = (size_t)gn + (size_t)gn / 2 + 1024;   /* >= fixed-Huffman worst case (~input*9/8) */
        uint8_t *out = kmalloc(ocap);
        long dl = out ? gz_deflate(in, (int)gn, out, (int)ocap) : -1;   /* empty input is a valid gzip */
        if (dl > 0 && vfs_write(outname, out, (unsigned long)dl) < 0) dl = -1;
        if (out) kfree(out);
        kfree(in);                          /* `in` is always allocated here: free unconditionally */
        r->rax = (uint64_t)(int64_t)dl;
        break;
    }
    case SYS_unzip: {
        const char *zn = (const char *)r->rdi;
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        uint8_t *zbuf; long zl = read_whole_file(zn, &zbuf);   /* the whole .zip (was a fixed 1MB read) */
        uint8_t *scr = (zl >= 0) ? kmalloc(1048576) : 0;       /* one decompressed entry at a time (<= 1 MB) */
        if (zl < 0 || !scr) { if (zl >= 0) kfree(zbuf); if (scr) kfree(scr); r->rax = (uint64_t)-1; break; }
        struct unzip_ctx uc = { 0 };
        int cnt = zip_extract(zbuf, (int)zl, unzip_emit, &uc, scr, 1048576);
        kfree(zbuf); kfree(scr);
        r->rax = (uint64_t)(int64_t)(cnt < 0 ? -1 : uc.written);   /* files actually written */
        break;
    }
    case SYS_untar: {
        const char *tn = (const char *)r->rdi;
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        uint8_t *buf; long fl = read_whole_file(tn, &buf);   /* the whole .tar/.tar.gz (was a fixed 1MB read) */
        if (fl <= 0) { if (fl == 0) kfree(buf); r->rax = (uint64_t)-1; break; }
        struct unzip_ctx uc = { 0 };
        int cnt;
        if (fl > 2 && buf[0] == 0x1f && buf[1] == 0x8b) {   /* .tar.gz: gunzip the tar first */
            uint8_t *tar = kmalloc(4194304);    /* decompressed tar (<= 4 MB) */
            if (!tar) { kfree(buf); r->rax = (uint64_t)-1; break; }
            int tl = gz_inflate(buf, (int)fl, tar, 4194304);
            cnt = tl > 0 ? tar_extract(tar, tl, unzip_emit, &uc) : -1;
            kfree(tar);
        } else {
            cnt = tar_extract(buf, (int)fl, unzip_emit, &uc);
        }
        kfree(buf);
        r->rax = (uint64_t)(int64_t)(cnt < 0 ? -1 : uc.written);
        break;
    }
    case SYS_crypt: {
        const char *name = (const char *)r->rdi, *pass = (const char *)r->rsi;
        if (!ustr(r->rdi) || !ustr(r->rsi)) { r->rax = (uint64_t)-1; break; }
        uint8_t *cbuf; long cn = read_whole_file(name, &cbuf);   /* whole file (was capped at 16KB) */
        if (cn < 0) { r->rax = (uint64_t)-1; break; }            /* missing / >=32MB / OOM */
        uint8_t kd[32]; sha256((const uint8_t *)pass, strlen(pass), kd);  /* key||nonce */
        aes128_ctr(cbuf, (size_t)cn, kd, kd + 16);
        long w = vfs_write(name, cbuf, (unsigned long)cn);
        kfree(cbuf);
        r->rax = (uint64_t)(int64_t)w;
        break;
    }
    case SYS_df: {
        uint64_t fb, tb; vfs_df(&fb, &tb);
        char *b = (char *)r->rsi; int max = (int)r->rdx, p = 0;
        if (max > 0 && !ubuf(r->rsi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }
        p = sappend(b, p, max, "  disk: ");
        p = snum(b, p, max, fb / 1024);
        p = sappend(b, p, max, " KiB free / ");
        p = snum(b, p, max, tb / 1024);
        p = sappend(b, p, max, " KiB total\n");
        if (p < max) b[p] = 0;
        r->rax = (uint64_t)p;
        break;
    }
    case SYS_ps: {
        task_info_t ti[16];
        int cnt = task_snapshot(ti, 16);
        char *b = (char *)r->rsi; int max = (int)r->rdx, p = 0;
        if (max > 0 && !ubuf(r->rsi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }
        static const char *st[5] = { "ready", "run  ", "block", "dead ", "stop " };
        for (int i = 0; i < cnt; i++) {
            if (ti[i].state == 3) continue;          /* skip dead tasks */
            p = sappend(b, p, max, "  [");
            p = snum(b, p, max, (uint64_t)ti[i].id);
            p = sappend(b, p, max, "] ");
            p = sappend(b, p, max, st[(unsigned)ti[i].state < 5 ? ti[i].state : 0]);
            p = sappend(b, p, max, "  ");
            p = sappend(b, p, max, ti[i].proc ? app_title((app_t *)ti[i].proc) : "(kernel)");
            p = sappend(b, p, max, "\n");
        }
        if (p < max) b[p] = 0;
        r->rax = (uint64_t)p;
        break;
    }
    case SYS_history:
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)app_sys_history((char *)r->rsi, (int)r->rdx);
        break;
    case SYS_resolve: {
        __asm__ volatile("sti");
        const char *host = (const char *)r->rdi;
        char *buf = (char *)r->rsi; int max = (int)r->rdx;
        uint8_t ip[4];
        if (max <= 0) { r->rax = (uint64_t)-1; break; }
        if (!ustr(r->rdi) || !ubuf(r->rsi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }  /* host + result buf */
        if (dns_resolve(host, ip) == 0) {
            int n = 0;
            for (int i = 0; i < 4; i++) {
                n = snum(buf, n, max, ip[i]);
                if (n + 1 < max) buf[n++] = (i < 3) ? '.' : '\n';   /* keep room for NUL */
            }
            buf[n] = 0; r->rax = 0;
        } else r->rax = (uint64_t)-1;
        break;
    }
    case SYS_reboot:
        acpi_reboot();                     /* ACPI reset register, else 8042 pulse */
        for (;;) __asm__ volatile("hlt");
        break;
    case SYS_poweroff:
        acpi_poweroff();                   /* enter ACPI S5: power the machine off */
        for (;;) __asm__ volatile("hlt");
        break;
    case SYS_kill: {                       /* ask the app with this pid to close (cooperative, like the X button) */
        int pid = (int)r->rdi;
        task_info_t ti[24];
        int cnt = task_snapshot(ti, 24);
        r->rax = (uint64_t)-1;
        for (int i = 0; i < cnt; i++)
            if (ti[i].id == pid && ti[i].proc && ti[i].state != 3) {
                app_request_kill((app_t *)ti[i].proc);
                r->rax = 0;
                break;
            }
        break;
    }
    case SYS_sbrk:
        r->rax = app_sbrk((long)r->rdi);   /* grow the heap; old break, or (uint64_t)-1 */
        break;
    case SYS_mmap:
        r->rax = app_mmap(r->rdi);         /* reserve a demand-paged anon region; base VA or 0 */
        break;
    case SYS_munmap:
        r->rax = (uint64_t)(int64_t)app_munmap(r->rdi, r->rsi);
        break;
    case SYS_madvise:                      /* (addr, len, advice) -> MADV_DONTNEED reclaims resident anon pages */
        r->rax = (uint64_t)(int64_t)app_madvise(r->rdi, r->rsi, (int)r->rdx);
        break;
    case SYS_swapout:                      /* (addr, len) -> page out anon pages to swap */
        __asm__ volatile("sti");           /* the disk writes may wait on an IRQ (virtio) */
        r->rax = (uint64_t)(int64_t)app_swap_out(r->rdi, r->rsi);
        break;
    case SYS_shm_open:                     /* (name, size) -> map a named shared-memory object */
        if (!ustr(r->rdi)) { r->rax = 0; break; }
        r->rax = app_shm_open((const char *)r->rdi, r->rsi);
        break;
    case SYS_futex:                        /* (uaddr, op, val) -> FUTEX_WAIT/WAKE */
        r->rax = (uint64_t)(int64_t)app_futex(r->rdi, (int)r->rsi, (int)r->rdx);
        break;
    case SYS_losetup: {                    /* (path) -> mount a FS image file as a loop device */
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        uint64_t cap = 4u * 1024 * 1024;   /* image size limit */
        uint8_t *img = kmalloc(cap);
        if (!img) { r->rax = (uint64_t)-1; break; }
        long n = vfs_read((const char *)r->rdi, img, cap);
        int idx = (n > 1024) ? blockdev_losetup(img, (uint64_t)n) : -1;
        if (idx < 0) kfree(img);           /* not a recognised FS -> losetup kept nothing */
        r->rax = (uint64_t)(int64_t)idx;
        break;
    }
    case SYS_ringbuf:                      /* (len): a magic mirrored ring buffer; base VA or 0 */
        r->rax = app_ringbuf(r->rdi);
        break;
    case SYS_mprotect:                     /* (addr, len, prot): change R/W/X of a mapped range */
        r->rax = (uint64_t)(int64_t)app_mprotect(r->rdi, r->rsi, (int)r->rdx);
        break;
    case SYS_bind:                         /* (from, to): graft FROM's subtree onto the path TO */
        if (!ustr(r->rdi) || !ustr(r->rsi)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)vfs_bind((const char *)r->rdi, (const char *)r->rsi);
        break;
    case SYS_unshare:                      /* detach into a private mount namespace (M1122) */
        r->rax = (uint64_t)(int64_t)vfs_unshare();
        break;
    case SYS_singlestep:                   /* hardware single-step the next n user instructions (M1123) */
        r->rax = (uint64_t)(int64_t)app_singlestep(r, (int)r->rdi);
        break;
    case SYS_seccomp:                      /* child: trap syscall `nr` to the supervisor (M1124) */
        r->rax = (uint64_t)(int64_t)app_seccomp_arm((int)r->rdi);
        break;
    case SYS_seccomp_wait: {               /* supervisor: block until the child parks; fill ev[4] */
        uint64_t *uev = (uint64_t *)r->rsi;
        if (!ubuf(r->rsi, 4 * sizeof(uint64_t))) { r->rax = (uint64_t)-1; break; }
        uint64_t kev[4] = {0,0,0,0};
        long rc = app_seccomp_wait((int)r->rdi, kev);
        if (rc > 0) for (int i = 0; i < 4; i++) uev[i] = kev[i];   /* copy out (supervisor's space active) */
        r->rax = (uint64_t)rc;
        break;
    }
    case SYS_seccomp_reply:                /* supervisor: deliver the verdict (M1124) */
        r->rax = (uint64_t)(int64_t)app_seccomp_reply((int)r->rdi, (int)r->rsi, (long)r->rdx);
        break;
    case SYS_fswait: {                     /* block until one of n paths is readable (select/poll, M1125) */
        const char *p = (const char *)r->rdi; int n = (int)r->rsi; long timeout = (long)r->rdx;
        const char *names[8];
        if (n < 1 || n > 8) { r->rax = (uint64_t)-1; break; }
        int bad = 0;
        for (int i = 0; i < n; i++) {       /* validate + collect the NUL-separated names */
            if (!ustr((uint64_t)(uintptr_t)p)) { bad = 1; break; }
            names[i] = p; while (*p) p++; p++;   /* advance past this name's NUL */
        }
        if (bad) { r->rax = (uint64_t)-1; break; }
        __asm__ volatile("sti");            /* the poll loop sleeps on the timer */
        uint64_t start = timer_ms();
        long idx = -1;
        for (;;) {
            for (int i = 0; i < n; i++) if (vfs_ready(names[i])) { idx = i; break; }
            if (idx >= 0) break;
            if (timeout >= 0 && (long)(timer_ms() - start) >= timeout) break;   /* -1 = timeout */
            task_sleep_ms(10);              /* off-CPU poll interval */
        }
        r->rax = (uint64_t)idx;
        break;
    }
    case SYS_signalfd:                     /* route masked signals to /proc/self/sigfd (M1126) */
        r->rax = (uint64_t)(int64_t)app_signalfd((uint32_t)r->rdi);
        break;
    case SYS_signal:                       /* (signo, handler, restorer): install a handler */
        app_signal_set((int)r->rdi, r->rsi, r->rdx);
        r->rax = 0;
        break;
    case SYS_raise:                        /* (signo): queue the signal; delivered to a handler at this
                                            * syscall's return (app_deliver_pending tail), or left pending
                                            * for signalfd if it has no handler (M1126). */
        r->rax = 0;
        app_request_signal((app_t *)app_current(), (int)r->rdi);
        break;
    case SYS_sigreturn:                    /* return from a handler: restore the saved context */
        app_sigreturn(r);
        break;
    case SYS_alarm:                        /* (ticks): arm a periodic SIGALRM (0 = disarm) */
        app_set_alarm(r->rdi);
        r->rax = 0;
        break;
    case SYS_uptime_ms:
        r->rax = timer_ms();               /* monotonic milliseconds since boot */
        break;
    case SYS_gfx_init:
        r->rax = (uint64_t)(int64_t)app_gfx_init((int)r->rdi, (int)r->rsi);
        break;
    case SYS_gfx_blit:
        r->rax = (uint64_t)(int64_t)app_gfx_blit((const uint32_t *)r->rdi);
        break;
    case SYS_setkbmode:
        app_set_rawkb((int)r->rdi);
        break;
    case SYS_caret:
        app_set_caret((int)r->rdi);
        break;
    case SYS_getkbevent:
        r->rax = (uint64_t)(int64_t)app_sys_getkbevent();
        break;
    case SYS_pcm:
        if ((int)r->rsi > 0 && !ubuf(r->rdi, (uint64_t)(int)r->rsi * 4)) { r->rax = (uint64_t)-1; break; }  /* nframes stereo 16-bit = 4 B each */
        __asm__ volatile("sti");           /* audio_play blocks on the timer */
        audio_play((const int16_t *)r->rdi, (int)r->rsi);
        break;
    case SYS_pcm_stream:
        if ((int)r->rsi > 0 && !ubuf(r->rdi, (uint64_t)(int)r->rsi * 4)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)audio_stream_write((const int16_t *)r->rdi, (int)r->rsi);
        break;
    case SYS_pcm_avail:
        r->rax = (uint64_t)(int64_t)audio_stream_avail();
        break;
    case SYS_mouse:
        r->rax = (uint64_t)app_get_mouse();
        break;
    case SYS_mouse_rel:
        r->rax = (uint64_t)app_get_mouse_rel();
        break;
    case SYS_playbg: {
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        __asm__ volatile("sti");                  /* the file read can be slow; stay preemptible */
        uint8_t *wb = kmalloc(8 * 1024 * 1024);
        long n = wb ? vfs_read((const char *)r->rdi, wb, 8 * 1024 * 1024) : -1;
        int rc = (n > 0) ? audio_play_wav_bg(wb, (int)n) : -1;
        if (wb) kfree(wb);                         /* decoded copy is independent of this buffer */
        r->rax = (uint64_t)(int64_t)rc;
        break;
    }
    case SYS_audiostop:
        audio_stop_bg();
        break;
    case SYS_playwav: {
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        __asm__ volatile("sti");
        uint8_t *wb = kmalloc(8 * 1024 * 1024);   /* the .wav file (<= 8 MB) */
        long n = wb ? vfs_read((const char *)r->rdi, wb, 8 * 1024 * 1024) : -1;
        int rc = (n > 0) ? audio_play_wav(wb, (int)n) : -1;
        if (wb) kfree(wb);
        r->rax = (uint64_t)(int64_t)rc;
        break;
    }
    case SYS_savebmp: {
        /* rdi=name, rsi=pixels, rdx=w, r10=h: save a w*h 0x00RRGGBB buffer as a
         * 24-bit BMP. Validate the name string + the pixel buffer (w*h*4 bytes,
         * the app's own pages) and require positive, sane dimensions before
         * touching either — same ring3->ring0 pointer boundary as the others. */
        int w = (int)r->rdx, h = (int)r->r10;
        if (!ustr(r->rdi) || w <= 0 || h <= 0 || (long)w * h > 4000000L ||
            !ubuf(r->rsi, (uint64_t)w * (uint64_t)h * 4)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)fb_save_bmp_buf((const char *)r->rdi,
                                                    (const uint32_t *)r->rsi, w, h);
        break;
    }
    case SYS_setwall:
        /* rdi=name: load that image as the desktop wallpaper. Validate the name
         * string (the app's own pages) before the kernel reads it; the decode +
         * atomic buffer swap happen with IF=0, so the WM render task never sees
         * a half-updated wallpaper, and a failed load keeps the current one. */
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)desktop_set_wallpaper((const char *)r->rdi);
        break;
    case SYS_lspci: {
        /* rdi=buf, rsi=len: format the PCI device list as text into the caller's
         * buffer. Validate the buffer lies in the app's own pages before writing
         * (the ring3->ring0 pointer boundary), exactly like SYS_ps / SYS_df. */
        int max = (int)r->rsi;
        if (max <= 0 || !ubuf(r->rdi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)pci_format((char *)r->rdi, max);
        break;
    }
    case SYS_lsblk: {
        /* rdi=buf, rsi=len: format the block-device + FAT32-volume listing into
         * the caller's buffer. Validate it lies in the app's own pages, like
         * SYS_lspci / SYS_ps / SYS_df. */
        int max = (int)r->rsi;
        if (max <= 0 || !ubuf(r->rdi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)blockdev_format((char *)r->rdi, max);
        break;
    }
    case SYS_mounts: {                     /* rdi=buf, rsi=len: list the read-only /diskN mounts */
        int max = (int)r->rsi;
        if (max <= 0 || !ubuf(r->rdi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)blockdev_mounts_format((char *)r->rdi, max);
        break;
    }
    case SYS_getrandom: {                  /* rdi=buf, rsi=len: fill with CSPRNG bytes */
        uint64_t len = r->rsi;
        if (len == 0 || !ubuf(r->rdi, len)) { r->rax = (uint64_t)-1; break; }
        random_bytes((void *)r->rdi, (size_t)len);
        r->rax = len;
        break;
    }
    case SYS_pledge: {                     /* rdi = promise string ("stdio rpath ...") */
        if (!ustr(r->rdi)) { r->rax = (uint64_t)-1; break; }
        uint32_t mask;
        if (app_pledge_parse((const char *)r->rdi, &mask) < 0) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)app_pledge(app_current(), mask);
        break;
    }
    case SYS_unveil: {                     /* rdi = path (0 to lock), rsi = perms string ("rwc") */
        if (r->rdi == 0) { r->rax = (uint64_t)(int64_t)app_unveil(self, 0, 0); break; }  /* lock */
        if (!ustr(r->rdi) || (r->rsi && !ustr(r->rsi))) { r->rax = (uint64_t)-1; break; }
        uint32_t perms = app_unveil_parse((const char *)r->rsi);
        r->rax = (uint64_t)(int64_t)app_unveil(self, (const char *)r->rdi, perms);
        break;
    }
    case SYS_symlink:                      /* rdi = linkpath (under /tmp), rsi = target */
        if (!ustr(r->rdi) || !ustr(r->rsi)) { r->rax = (uint64_t)-1; break; }
        if (!app_unveil_ok(self, (const char *)r->rdi, 1)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)vfs_symlink((const char *)r->rdi, (const char *)r->rsi);
        break;
    case SYS_jail: {                       /* rdi=prog, rsi=promises, rdx=path (0=none): spawn pre-confined */
        if (!ustr(r->rdi) || !ustr(r->rsi) || (r->rdx && !ustr(r->rdx))) { r->rax = (uint64_t)-1; break; }
        uint32_t mask;
        if (app_pledge_parse((const char *)r->rsi, &mask) < 0) { r->rax = (uint64_t)-1; break; }
        app_jail_next(mask, (const char *)r->rdx);
        r->rax = (uint64_t)(int64_t)app_spawn_named((const char *)r->rdi);
        break;
    }
    case SYS_exit:
        app_sys_exit((int)r->rdi);         /* records the exit status, marks app dead + task_exit; no return */
        break;
    default:
        kprintf("[kernel] unknown syscall %lu\n", r->rax);
        r->rax = (uint64_t)-1;
        break;
    }

sc_done:                      /* seccomp-notify lands here after denying/emulating a syscall (M1124) */
    if (traced) {             /* strace (M1084): one line to dmesg + the readable ring (M1118) */
        kprintf("[strace %d] %s(0x%lx, 0x%lx, 0x%lx) = 0x%lx\n",
                app_sys_getpid(), syscall_name(tr_nr), tr_a, tr_b, tr_c, r->rax);
        strace_record(task_current_id(), syscall_name(tr_nr), tr_a, tr_b, tr_c, r->rax);  /* tag with the task id, matching /proc/<pid> */
    }

    app_deliver_pending(r);   /* an async signal (e.g. Ctrl-C->SIGINT) raised during the syscall (M1083) */
}
