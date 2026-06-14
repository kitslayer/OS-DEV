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
