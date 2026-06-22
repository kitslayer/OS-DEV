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
#include "ac97.h"
#include <stdint.h>

/* Validate a user-supplied syscall pointer argument: the range [p, p+n) must
 * lie entirely within the calling app's own user (PTE_USER) pages. A syscall
 * runs in ring 0 with the app's CR3 active, where kernel memory is mapped and
 * writable — so without this an app could hand a kernel pointer to a handler
 * and have the kernel read or (worse) write its own memory. On failure the
 * handler returns -1 instead of touching the bogus address. `n` is the exact
 * number of bytes the handler will access through the pointer. */
static int ubuf(uint64_t p, uint64_t n) { return vmm_user_ok(p, n); }

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

void syscall_dispatch(struct registers *r) {
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
        if (!ubuf((uint64_t)buf, max)) { r->rax = (uint64_t)-1; break; }
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
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)vfs_write((const char *)r->rdi, (const void *)r->rsi, r->rdx);
        break;
    case SYS_delete:
        r->rax = (uint64_t)vfs_remove((const char *)r->rdi);
        break;
    case SYS_spawn: {
        const char *nm = (const char *)r->rdi;
        const char *arg = (const char *)r->rsi;    /* optional launch arg (e.g. a filename for the editor) */
        int rc = (arg && arg[0]) ? app_spawn_named_arg(nm, arg) : app_spawn_named(nm);  /* a built-in program? */
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
        if (!ubuf(r->rdx, r->r10)) { r->rax = (uint64_t)-1; break; }   /* response buffer */
        __asm__ volatile("sti");           /* TCP needs the timer running */
        r->rax = (uint64_t)(int64_t)http_get((const char *)r->rdi,
                                             (const char *)r->rsi,
                                             (char *)r->rdx, (int)r->r10);
        break;
    case SYS_https:
        if (!ubuf(r->rdx, r->r10)) { r->rax = (uint64_t)-1; break; }   /* response buffer */
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
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }   /* result buffer */
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
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)(int64_t)vfs_tree((char *)r->rsi, (int)r->rdx);
        break;
    case SYS_pollkey:
        r->rax = (uint64_t)(int64_t)app_sys_pollkey();
        break;
    case SYS_find:
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }   /* result buffer (rdi search term left to phase 2) */
        r->rax = (uint64_t)(int64_t)vfs_find((const char *)r->rdi,
                                             (char *)r->rsi, (int)r->rdx);
        break;
    case SYS_sha256: {
        if ((int)r->rdx < 65) { r->rax = (uint64_t)-1; break; }   /* need room for 64 hex + NUL */
        if (!ubuf(r->rsi, 65)) { r->rax = (uint64_t)-1; break; }  /* hex output buffer */
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
        if (!ubuf(r->rsi, 129)) { r->rax = (uint64_t)-1; break; }  /* hex output buffer */
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
        int L = 0; while (sn[L]) L++;
        int png = (L >= 4 && sn[L-4]=='.' && (sn[L-3]|32)=='p' && (sn[L-2]|32)=='n' && (sn[L-1]|32)=='g');
        r->rax = (uint64_t)(int64_t)(png ? fb_save_png(sn) : fb_save_bmp(sn));
        break;
    }
    case SYS_gunzip: {
        const char *insrc = (const char *)r->rdi, *outname = (const char *)r->rsi;
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
        if (out) kfree(out); kfree(in);
        r->rax = (uint64_t)(int64_t)dl;
        break;
    }
    case SYS_gzip: {
        const char *insrc = (const char *)r->rdi, *outname = (const char *)r->rsi;
        uint8_t *in = 0; long gn = read_whole_file(insrc, &in);   /* whole input, not a 256KB prefix */
        if (gn < 0) { r->rax = (uint64_t)-1; break; }
        size_t ocap = (size_t)gn + (size_t)gn / 2 + 1024;   /* >= fixed-Huffman worst case (~input*9/8) */
        uint8_t *out = kmalloc(ocap);
        long dl = out ? gz_deflate(in, (int)gn, out, (int)ocap) : -1;   /* empty input is a valid gzip */
        if (dl > 0 && vfs_write(outname, out, (unsigned long)dl) < 0) dl = -1;
        if (out) kfree(out); kfree(in);
        r->rax = (uint64_t)(int64_t)dl;
        break;
    }
    case SYS_unzip: {
        const char *zn = (const char *)r->rdi;
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
        if (!ubuf(r->rsi, r->rdx)) { r->rax = (uint64_t)-1; break; }
        r->rax = (uint64_t)app_sys_history((char *)r->rsi, (int)r->rdx);
        break;
    case SYS_resolve: {
        __asm__ volatile("sti");
        const char *host = (const char *)r->rdi;
        char *buf = (char *)r->rsi; int max = (int)r->rdx;
        uint8_t ip[4];
        if (max <= 0) { r->rax = (uint64_t)-1; break; }
        if (!ubuf(r->rsi, (uint64_t)max)) { r->rax = (uint64_t)-1; break; }
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
    case SYS_sbrk:
        r->rax = app_sbrk((long)r->rdi);   /* grow the heap; old break, or (uint64_t)-1 */
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
        __asm__ volatile("sti");           /* ac97_play blocks on the timer */
        ac97_play((const int16_t *)r->rdi, (int)r->rsi);
        break;
    case SYS_pcm_stream:
        r->rax = (uint64_t)(int64_t)ac97_stream_write((const int16_t *)r->rdi, (int)r->rsi);
        break;
    case SYS_pcm_avail:
        r->rax = (uint64_t)(int64_t)ac97_stream_avail();
        break;
    case SYS_mouse:
        r->rax = (uint64_t)app_get_mouse();
        break;
    case SYS_mouse_rel:
        r->rax = (uint64_t)app_get_mouse_rel();
        break;
    case SYS_playbg: {
        __asm__ volatile("sti");                  /* the file read can be slow; stay preemptible */
        uint8_t *wb = kmalloc(8 * 1024 * 1024);
        long n = wb ? vfs_read((const char *)r->rdi, wb, 8 * 1024 * 1024) : -1;
        int rc = (n > 0) ? ac97_play_wav_bg(wb, (int)n) : -1;
        if (wb) kfree(wb);                         /* decoded copy is independent of this buffer */
        r->rax = (uint64_t)(int64_t)rc;
        break;
    }
    case SYS_audiostop:
        ac97_stop_bg();
        break;
    case SYS_playwav: {
        __asm__ volatile("sti");
        uint8_t *wb = kmalloc(8 * 1024 * 1024);   /* the .wav file (<= 8 MB) */
        long n = wb ? vfs_read((const char *)r->rdi, wb, 8 * 1024 * 1024) : -1;
        int rc = (n > 0) ? ac97_play_wav(wb, (int)n) : -1;
        if (wb) kfree(wb);
        r->rax = (uint64_t)(int64_t)rc;
        break;
    }
    case SYS_exit:
        app_sys_exit();                    /* marks app dead + task_exit; no return */
        break;
    default:
        kprintf("[kernel] unknown syscall %lu\n", r->rax);
        r->rax = (uint64_t)-1;
        break;
    }
}
