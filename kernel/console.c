/*
 * console.c — console_putc + a small freestanding printf (kprintf).
 *
 * There is no libc here, so we write our own formatter. It supports the
 * conversions the kernel actually needs:
 *
 *   %s   string            %c   character        %%   literal percent
 *   %d/%i signed decimal    %u   unsigned decimal
 *   %x/%X hex (lower/upper)  %p   pointer (0x + 64-bit hex)
 *
 * with an optional field width and '0' padding flag (e.g. %08x), and the
 * length modifiers 'l' (long) and 'z' (size_t) — important because addresses
 * and sizes in a 64-bit kernel don't fit in an int.
 */
#include "console.h"
#include "vga.h"
#include "serial.h"
#include "fbcon.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static bool gfx_console;

/* ---- kernel log ring buffer (M1071) -----------------------------------------
 * Every byte that goes to the console is also captured into a fixed circular
 * buffer, so userspace can read the kernel log back as /proc/kmsg (a `dmesg`)
 * even after it has scrolled off-screen. Lock-free by design: a single
 * monotonically-increasing head index + plain byte writes, so it is safe to
 * call from IRQ / kprintf / panic context with no risk of deadlock. A reader
 * may see a torn tail byte under a concurrent write; that is acceptable for a
 * log. */
#define KLOG_SIZE 65536
static char klog[KLOG_SIZE];
static volatile uint32_t klog_head;          /* total bytes ever written (wraps the buffer) */

static void klog_putc(char c) {
    klog[klog_head % KLOG_SIZE] = c;
    klog_head++;
}

/* Copy the most recent log bytes (oldest-first) into out[max], NUL-terminated.
 * Returns the number of bytes written (excluding the NUL). */
int klog_copy(char *out, int max) {
    if (!out || max <= 1) return 0;
    uint32_t head  = klog_head;               /* snapshot the head once */
    uint32_t avail = head < KLOG_SIZE ? head : KLOG_SIZE;
    if (avail > (uint32_t)(max - 1)) avail = (uint32_t)(max - 1);
    uint32_t start = head - avail;            /* oldest byte we will return */
    int n = 0;
    for (uint32_t i = 0; i < avail; i++)
        out[n++] = klog[(start + i) % KLOG_SIZE];
    out[n] = 0;
    return n;
}

/* Append userspace bytes to the kernel log ring (the /dev/kmsg writer, M1216),
 * so init scripts / apps can log into `dmesg`. A trailing newline is ensured so
 * each write is its own log line. */
void klog_write(const char *buf, int n) {
    for (int i = 0; i < n; i++) klog_putc(buf[i]);
    if (n == 0 || buf[n - 1] != '\n') klog_putc('\n');
}

/* ---- console capture (M1870) ------------------------------------------------
 * A single-consumer sink so the network debug console (netcon.c) can capture the
 * output of a kprintf-based dumper (e.g. pci_enumerate) into a buffer and ship it
 * over the socket, instead of only to the screen/serial. Best-effort: another
 * core's kprintf during a capture window interleaves into the buffer, which is
 * fine for diagnostics. Bracket the buffer's lifetime with begin()/end(). */
static char *g_cap; static int g_cap_max, g_cap_n;
void console_capture_begin(char *buf, int max) { g_cap_n = 0; g_cap_max = max; g_cap = buf; }
int  console_capture_end(void)   { int n = g_cap_n; g_cap = 0; return n; }

void console_init(void) {
    serial_init();
    vga_init();
}

void console_enable_gfx(void) {
    gfx_console = true;
}

void console_putc(char c) {
    if (gfx_console)
        fbcon_putc(c);       /* framebuffer console */
    else
        vga_putc(c);         /* legacy VGA text mode */
    klog_putc(c);            /* capture into the kernel log ring (M1071) */
    if (g_cap && g_cap_n < g_cap_max - 1) g_cap[g_cap_n++] = c;   /* netcon capture (M1870) */
    if (c == '\n')
        serial_putc('\r');   /* terminals want CRLF; the screen doesn't care */
    serial_putc(c);
}

void console_write(const char *s) {
    for (; *s; s++)
        console_putc(*s);
}

/* ---- number formatting ------------------------------------------------- */

/* Print `value` in the given base, right-justified in `width` using `pad`. */
static void print_uint(uint64_t value, unsigned base, bool upper,
                       int width, char pad) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[32];
    int n = 0;

    if (value == 0)
        buf[n++] = '0';
    while (value) {
        buf[n++] = digits[value % base];
        value /= base;
    }

    for (int i = n; i < width; i++)   /* leading padding */
        console_putc(pad);
    while (n > 0)                     /* digits come out reversed */
        console_putc(buf[--n]);
}

/* ---- the formatter ----------------------------------------------------- */

void kvprintf(const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            console_putc(*fmt);
            continue;
        }

        fmt++;                                  /* skip '%' */

        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        int lng = 0;                            /* 0=int, 1=long, 2=size_t */
        if (*fmt == 'l') { lng = 1; fmt++; if (*fmt == 'l') fmt++; }
        else if (*fmt == 'z') { lng = 2; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) console_putc(*s++);
            break;
        }
        case 'c':
            console_putc((char)va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            long v = (lng == 0) ? va_arg(ap, int) : va_arg(ap, long);
            if (v < 0) { console_putc('-'); v = -v; }
            print_uint((uint64_t)v, 10, false, width, pad);
            break;
        }
        case 'u': {
            uint64_t v = (lng == 0) ? va_arg(ap, unsigned)
                                    : va_arg(ap, unsigned long);
            print_uint(v, 10, false, width, pad);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = (lng == 0) ? va_arg(ap, unsigned)
                                    : va_arg(ap, unsigned long);
            print_uint(v, 16, *fmt == 'X', width, pad);
            break;
        }
        case 'p':
            console_putc('0'); console_putc('x');
            print_uint((uint64_t)(uintptr_t)va_arg(ap, void *),
                       16, false, 16, '0');
            break;
        case '%':
            console_putc('%');
            break;
        default:                                /* unknown: print verbatim */
            console_putc('%');
            console_putc(*fmt);
            break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
