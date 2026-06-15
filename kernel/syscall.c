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
#include "vfs.h"
#include "rtc.h"
#include "speaker.h"
#include "pmm.h"
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
#include <stdint.h>

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

void syscall_dispatch(struct registers *r) {
    switch (r->rax) {
    case SYS_write:
        /* stdout goes to the calling app's window text grid */
        app_sys_write((const char *)r->rsi, (unsigned)r->rdx);
        r->rax = r->rdx;
        break;
    case SYS_read:
        /* a line of input from the app's window (blocks until Enter) */
        r->rax = (uint64_t)app_sys_read((char *)r->rsi, (unsigned)r->rdx);
        break;
    case SYS_getpid:
        r->rax = (uint64_t)app_sys_getpid();
        break;
    case SYS_list: {
        /* Format the root directory into the user buffer: "name  size\n". */
        char       *buf = (char *)r->rsi;
        uint64_t    max = r->rdx;
        vfs_dirent  ents[32];
        int         count = vfs_list(ents, 32);
        uint64_t    n = 0;
        for (int i = 0; i < count; i++) {
            for (int j = 0; ents[i].name[j] && n < max - 1; j++)
                buf[n++] = ents[i].name[j];
            while (n < max - 1 && (n == 0 || buf[n - 1] != '\n')) {
                buf[n++] = ' ';
                char num[12];
                int  ln = u32_to_dec(ents[i].size, num);
                for (int k = 0; k < ln && n < max - 1; k++) buf[n++] = num[k];
                if (n < max - 1) buf[n++] = '\n';
                break;
            }
        }
        buf[n] = '\0';
        r->rax = n;
        break;
    }
    case SYS_readfile: {
        const char *name = (const char *)r->rdi;
        void       *buf  = (void *)r->rsi;
        uint64_t    max  = r->rdx;
        r->rax = (uint64_t)vfs_read(name, buf, max);
        break;
    }
    case SYS_time: {
        /* write "YYYY-MM-DD HH:MM:SS\n" into the user buffer */
        char *buf = (char *)r->rsi;
        if (r->rdx >= 21) {
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
    case SYS_writefile:
        r->rax = (uint64_t)vfs_write((const char *)r->rdi, (const void *)r->rsi, r->rdx);
        break;
    case SYS_delete:
        r->rax = (uint64_t)vfs_remove((const char *)r->rdi);
        break;
    case SYS_spawn: {
        const char *nm = (const char *)r->rdi;
        int rc = app_spawn_named(nm);              /* a built-in program? */
        if (rc < 0) rc = app_spawn_from_file(nm);  /* else try loading it from disk */
        r->rax = (uint64_t)(int64_t)rc;
        break;
    }
    case SYS_sleep:
        __asm__ volatile("sti");           /* timer drives the wait */
        timer_wait(r->rdi / 10 + 1);
        break;
    case SYS_ping:
        __asm__ volatile("sti");           /* needs the timer for its timeout */
        r->rax = (uint64_t)(int64_t)net_ping_gateway();
        break;
    case SYS_pinghost:
        __asm__ volatile("sti");           /* DNS + ICMP both need the timer running */
        r->rax = (uint64_t)(int64_t)net_ping_host((const char *)r->rdi);
        break;
    case SYS_apps:
        r->rax = (uint64_t)(int64_t)app_list_names((char *)r->rdi, (int)r->rsi);
        break;
    case SYS_netinfo: {                    /* our IP/MAC/gateway/DNS as aligned text */
        char *b = (char *)r->rdi; int max = (int)r->rsi;
        if (max < 128) { r->rax = (uint64_t)-1; break; }   /* worst case ~96 B; require headroom */
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
        __asm__ volatile("sti");           /* TCP needs the timer running */
        r->rax = (uint64_t)(int64_t)http_get((const char *)r->rdi,
                                             (const char *)r->rsi,
                                             (char *)r->rdx, (int)r->r10);
        break;
    case SYS_https:
        __asm__ volatile("sti");           /* TLS/TCP need the timer running */
        r->rax = (uint64_t)(int64_t)tls_get((const char *)r->rdi,
                                            (const char *)r->rsi,
                                            (uint8_t *)r->rdx, (int)r->r10,
                                            (uint32_t)timer_ticks());
        break;
    case SYS_browse:
        app_browse((const char *)r->rdi);  /* WM opens the browser window */
        r->rax = 0;
        break;
    case SYS_js:
        __asm__ volatile("sti");           /* keep the timer live during long scripts */
        r->rax = (uint64_t)(int64_t)js_run((const char *)r->rdi,
                                           (char *)r->rsi, (int)r->rdx);
        break;
    case SYS_mkdir:
        r->rax = (uint64_t)(int64_t)vfs_mkdir((const char *)r->rdi);
        break;
    case SYS_chdir:
        r->rax = (uint64_t)(int64_t)vfs_chdir((const char *)r->rdi);
        break;
    case SYS_tree:
        r->rax = (uint64_t)(int64_t)vfs_tree((char *)r->rsi, (int)r->rdx);
        break;
    case SYS_pollkey:
        r->rax = (uint64_t)(int64_t)app_sys_pollkey();
        break;
    case SYS_find:
        r->rax = (uint64_t)(int64_t)vfs_find((const char *)r->rdi,
                                             (char *)r->rsi, (int)r->rdx);
        break;
    case SYS_sha256: {
        if ((int)r->rdx < 65) { r->rax = (uint64_t)-1; break; }   /* need room for 64 hex + NUL */
        static uint8_t fbuf[16384];
        long fn = vfs_read((const char *)r->rdi, fbuf, sizeof(fbuf));
        if (fn < 0) { r->rax = (uint64_t)-1; break; }
        uint8_t dg[32]; sha256(fbuf, (size_t)fn, dg);
        char *hx = (char *)r->rsi; const char *H = "0123456789abcdef";
        for (int i = 0; i < 32; i++) { hx[i*2] = H[dg[i]>>4]; hx[i*2+1] = H[dg[i]&0xF]; }
        hx[64] = 0; r->rax = 0;
        break;
    }
    case SYS_sha512: {
        if ((int)r->rdx < 129) { r->rax = (uint64_t)-1; break; }   /* need room for 128 hex + NUL */
        static uint8_t fbuf512[16384];                             /* hashes only the first 16 KB of a larger file (matches SYS_sha256) */
        long fn = vfs_read((const char *)r->rdi, fbuf512, sizeof(fbuf512));
        if (fn < 0) { r->rax = (uint64_t)-1; break; }
        uint8_t dg[64]; sha512(fbuf512, (size_t)fn, dg);
        char *hx = (char *)r->rsi; const char *H = "0123456789abcdef";
        for (int i = 0; i < 64; i++) { hx[i*2] = H[dg[i]>>4]; hx[i*2+1] = H[dg[i]&0xF]; }
        hx[128] = 0; r->rax = 0;
        break;
    }
    case SYS_crypt: {
        const char *name = (const char *)r->rdi, *pass = (const char *)r->rsi;
        static uint8_t cbuf[16384];
        long cn = vfs_read(name, cbuf, sizeof(cbuf));
        if (cn < 0) { r->rax = (uint64_t)-1; break; }
        if (cn >= (long)sizeof(cbuf)) {          /* too big for our buffer: refuse, */
            r->rax = (uint64_t)-2;               /* don't truncate (would lose data) */
            break;
        }
        uint8_t kd[32]; sha256((const uint8_t *)pass, strlen(pass), kd);  /* key||nonce */
        aes128_ctr(cbuf, (size_t)cn, kd, kd + 16);
        r->rax = (uint64_t)(int64_t)vfs_write(name, cbuf, (unsigned long)cn);
        break;
    }
    case SYS_df: {
        uint64_t fb, tb; vfs_df(&fb, &tb);
        char *b = (char *)r->rsi; int max = (int)r->rdx, p = 0;
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
        static const char *st[4] = { "ready", "run  ", "block", "dead " };
        for (int i = 0; i < cnt; i++) {
            if (ti[i].state == 3) continue;          /* skip dead tasks */
            p = sappend(b, p, max, "  [");
            p = snum(b, p, max, (uint64_t)ti[i].id);
            p = sappend(b, p, max, "] ");
            p = sappend(b, p, max, st[ti[i].state & 3]);
            p = sappend(b, p, max, "  ");
            p = sappend(b, p, max, ti[i].proc ? app_title((app_t *)ti[i].proc) : "(kernel)");
            p = sappend(b, p, max, "\n");
        }
        if (p < max) b[p] = 0;
        r->rax = (uint64_t)p;
        break;
    }
    case SYS_history:
        r->rax = (uint64_t)app_sys_history((char *)r->rsi, (int)r->rdx);
        break;
    case SYS_resolve: {
        __asm__ volatile("sti");
        const char *host = (const char *)r->rdi;
        char *buf = (char *)r->rsi; int max = (int)r->rdx;
        uint8_t ip[4];
        if (max <= 0) { r->rax = (uint64_t)-1; break; }
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
        outb(0x64, 0xFE);                  /* pulse the 8042 reset line */
        for (;;) __asm__ volatile("hlt");
        break;
    case SYS_exit:
        app_sys_exit();                    /* marks app dead + task_exit; no return */
        break;
    default:
        kprintf("[kernel] unknown syscall %lu\n", r->rax);
        r->rax = (uint64_t)-1;
        break;
    }
}
