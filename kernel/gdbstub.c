/*
 * gdbstub.c — a GDB Remote Serial Protocol (RSP) stub, so a host `gdb` can
 * source-level debug this kernel over a serial port: `target remote`, then
 * `info registers`, `x/`, `bt`, `continue`. (M1204)
 *
 * Read-only MVP: `?` (stop reason), `g` (read registers), `m` (read memory),
 * `c`/`D`/`k` (continue/detach). Unsupported packets get the empty response,
 * which gdb treats as "not supported" and works around. Breakpoints (`Z0`),
 * single-step (`s`), and register/memory writes (`G`/`M`) are the follow-on.
 *
 * Wiring: enabled by the multiboot command line (`-append gdbstub`); when on,
 * kmain executes an `int3`, and the #BP handler hands the trap frame to
 * gdbstub_serve() — so gdb attaches with a real register snapshot. The stub
 * talks on COM2 (0x2F8), leaving COM1 as the normal kprintf console.
 *
 * The RSP codec (checksum / framing / hex / register marshalling / the command
 * handler) is pure over buffers, so tests/gdbstub exercises it on the host with
 * no serial or QEMU (the verbatim-extraction pattern).
 */
#include "interrupts.h"      /* struct registers */
#include <stdint.h>
#include <stddef.h>

static char hexc(int v) { return "0123456789abcdef"[v & 0xf]; }
static int  unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* append `bytes` of `v` as little-endian hex (the RSP register/memory encoding) */
static int put_hex_le(char *out, int p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        out[p++] = hexc(b >> 4);
        out[p++] = hexc(b & 0xf);
    }
    return p;
}

uint8_t gdb_cksum(const char *p, int n) {
    uint8_t s = 0;
    for (int i = 0; i < n; i++) s += (uint8_t)p[i];
    return s;
}

/* Marshal the trap frame into GDB's amd64 `g`-packet order: rax,rbx,rcx,rdx,
 * rsi,rdi,rbp,rsp, r8..r15, rip (all 64-bit), then eflags,cs,ss,ds,es,fs,gs
 * (32-bit). ds..gs aren't saved by the ISR, so report the kernel data selector
 * / 0. Returns the hex length. */
int gdb_reg_packet(const struct registers *r, char *out) {
    int p = 0;
    p = put_hex_le(out, p, r->rax, 8); p = put_hex_le(out, p, r->rbx, 8);
    p = put_hex_le(out, p, r->rcx, 8); p = put_hex_le(out, p, r->rdx, 8);
    p = put_hex_le(out, p, r->rsi, 8); p = put_hex_le(out, p, r->rdi, 8);
    p = put_hex_le(out, p, r->rbp, 8); p = put_hex_le(out, p, r->rsp, 8);
    p = put_hex_le(out, p, r->r8,  8); p = put_hex_le(out, p, r->r9,  8);
    p = put_hex_le(out, p, r->r10, 8); p = put_hex_le(out, p, r->r11, 8);
    p = put_hex_le(out, p, r->r12, 8); p = put_hex_le(out, p, r->r13, 8);
    p = put_hex_le(out, p, r->r14, 8); p = put_hex_le(out, p, r->r15, 8);
    p = put_hex_le(out, p, r->rip, 8);
    p = put_hex_le(out, p, r->rflags & 0xffffffffu, 4);   /* eflags */
    p = put_hex_le(out, p, r->cs, 4);  p = put_hex_le(out, p, r->ss, 4);
    p = put_hex_le(out, p, 0x10, 4);   p = put_hex_le(out, p, 0x10, 4);  /* ds, es */
    p = put_hex_le(out, p, 0, 4);      p = put_hex_le(out, p, 0, 4);     /* fs, gs */
    out[p] = 0;
    return p;
}

/* parse "m<addr>,<len>" (hex). 1 on success. */
static int parse_m(const char *c, uint64_t *addr, uint64_t *len) {
    *addr = 0; *len = 0;
    const char *s = c + 1; int any = 0, h;
    while (*s && *s != ',') { if ((h = unhex(*s)) < 0) return 0; *addr = (*addr << 4) | (unsigned)h; s++; any = 1; }
    if (*s != ',' || !any) return 0;
    s++; any = 0;
    while (*s) { if ((h = unhex(*s)) < 0) return 0; *len = (*len << 4) | (unsigned)h; s++; any = 1; }
    return any;
}

/* Handle one RSP command, writing the response *payload* into resp (NUL-term).
 * `readb` reads one byte of target memory (the kernel passes a bounds-checked
 * reader; the host test stubs it). Returns 1 if the inferior should resume
 * (continue/detach/kill — exit the serve loop), else 0. */
int gdb_handle(const char *cmd, char *resp, int rmax, const struct registers *r,
               int (*readb)(uint64_t addr, uint8_t *out)) {
    resp[0] = 0;
    switch (cmd[0]) {
    case '?':                                   /* why did we stop? SIGTRAP. */
        resp[0] = 'S'; resp[1] = '0'; resp[2] = '5'; resp[3] = 0;
        return 0;
    case 'g':                                   /* read all registers */
        gdb_reg_packet(r, resp);
        return 0;
    case 'm': {                                 /* read memory: m<addr>,<len> */
        uint64_t a, l;
        if (!parse_m(cmd, &a, &l)) { resp[0] = 'E'; resp[1] = '0'; resp[2] = '1'; resp[3] = 0; return 0; }
        if (l * 2 >= (uint64_t)(rmax - 1)) l = (uint64_t)(rmax - 1) / 2;
        int p = 0;
        for (uint64_t i = 0; i < l; i++) {
            uint8_t b;
            if (!readb(a + i, &b)) { if (p == 0) { resp[0]='E'; resp[1]='0'; resp[2]='e'; resp[3]=0; return 0; } break; }
            resp[p++] = hexc(b >> 4); resp[p++] = hexc(b & 0xf);
        }
        resp[p] = 0;
        return 0;
    }
    case 'c': case 'k':                         /* continue / kill -> resume */
        return 1;
    case 'D':                                   /* detach -> ack then resume */
        resp[0] = 'O'; resp[1] = 'K'; resp[2] = 0;
        return 1;
    default:                                    /* unsupported -> empty (gdb copes) */
        return 0;
    }
}

#ifndef GDBSTUB_HOST_TEST
#include "io.h"              /* inb/outb */

#define COM2 0x2F8
static int gdb_armed;        /* set when `-append gdbstub` was seen */

void gdbstub_arm(void)     { gdb_armed = 1; }
int  gdbstub_armed(void)   { return gdb_armed; }

static void com2_init(void) {
    outb(COM2 + 1, 0x00);    /* no interrupts: the stub polls */
    outb(COM2 + 3, 0x80);    /* DLAB */
    outb(COM2 + 0, 0x03);    /* 38400 baud */
    outb(COM2 + 1, 0x00);
    outb(COM2 + 3, 0x03);    /* 8N1 */
    outb(COM2 + 2, 0xC7);    /* FIFO */
    outb(COM2 + 4, 0x0B);
}
static char com2_getc(void) { while (!(inb(COM2 + 5) & 0x01)) { } return (char)inb(COM2); }
static void com2_putc(char c) { while (!(inb(COM2 + 5) & 0x20)) { } outb(COM2, (uint8_t)c); }

/* Bounds-checked target-memory read: only the kernel's higher half + the low
 * identity map, so a stray `m` from gdb can't fault the stub. */
static int kreadb(uint64_t a, uint8_t *out) {
    if (a < 0x1000) return 0;                                  /* null page */
    if (a >= 0x100000000ull && a < 0xFFFF800000000000ull) return 0;  /* the non-canonical / unmapped gap */
    *out = *(volatile uint8_t *)(uintptr_t)a;
    return 1;
}

static void send_pkt(const char *payload) {
    com2_putc('$');
    uint8_t ck = 0;
    for (const char *p = payload; *p; p++) { com2_putc(*p); ck += (uint8_t)*p; }
    com2_putc('#'); com2_putc(hexc(ck >> 4)); com2_putc(hexc(ck & 0xf));
    (void)com2_getc();       /* swallow the '+'/'-' ack */
}

/* Serve gdb from the trap frame `r` until it continues/detaches. */
void gdbstub_serve(struct registers *r) {
    static char pkt[2200], resp[2200];
    com2_init();
    for (;;) {
        char c;
        do { c = com2_getc(); } while (c != '$');       /* resync to a packet start */
        int n = 0;
        while ((c = com2_getc()) != '#' && n < (int)sizeof pkt - 1) pkt[n++] = c;
        pkt[n] = 0;
        com2_getc(); com2_getc();                        /* the 2 checksum hex chars */
        com2_putc('+');                                  /* ack the packet */
        int resume = gdb_handle(pkt, resp, (int)sizeof resp, r, kreadb);
        if (resp[0] || !resume) send_pkt(resp);          /* reply (empty if unsupported) */
        if (resume) break;
    }
}
#endif
