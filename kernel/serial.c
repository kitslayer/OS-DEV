/*
 * serial.c — the 16550 UART on COM1 (port 0x3F8).
 *
 * The serial port is the kernel's lifeline before we have a screen worth
 * looking at: QEMU pipes it straight to our terminal (`-serial stdio`), so
 * `make test` can confirm what the kernel did without any graphics at all.
 *
 * A UART is configured by writing a handful of its registers. The quirk is
 * that two of those registers are reused as the baud-rate "divisor latch" when
 * the DLAB bit (bit 7 of the Line Control Register) is set.
 */
#include "serial.h"
#include "io.h"
#include "keyboard.h"      /* input_push */
#include "interrupts.h"

#define COM1 0x3F8

/* register offsets from the base port */
#define UART_DATA        0  /* DLAB=0: data | DLAB=1: divisor low byte  */
#define UART_INT_ENABLE  1  /* DLAB=0: interrupt enable | DLAB=1: div hi */
#define UART_FIFO_CTRL   2
#define UART_LINE_CTRL   3  /* bit 7 = DLAB */
#define UART_MODEM_CTRL  4
#define UART_LINE_STATUS 5  /* bit 5 = transmit holding register empty   */

void serial_init(void) {
    outb(COM1 + UART_INT_ENABLE, 0x00); /* no interrupts (we poll for now) */

    outb(COM1 + UART_LINE_CTRL, 0x80);  /* DLAB=1: expose the divisor latch */
    outb(COM1 + UART_DATA,      0x03);  /* divisor = 3  -> 115200/3 = 38400 baud */
    outb(COM1 + UART_INT_ENABLE,0x00);  /* divisor high byte = 0 */

    outb(COM1 + UART_LINE_CTRL, 0x03);  /* DLAB=0, 8 data bits, no parity, 1 stop */
    outb(COM1 + UART_FIFO_CTRL, 0xC7);  /* enable + clear FIFOs, 14-byte threshold */
    outb(COM1 + UART_MODEM_CTRL,0x0B);  /* DTR + RTS + OUT2 (OUT2 gates IRQs later) */
}

static int tx_ready(void) {
    return inb(COM1 + UART_LINE_STATUS) & 0x20; /* transmit holding empty? */
}

/* BOUNDED transmit (M1919). This used to spin `while (!tx_ready())` forever, which
 * made a missing or wedged COM1 a single point of total failure: every kprintf
 * would hang, and since M1915 the console lock is held with interrupts DISABLED
 * across a line, so the whole machine would freeze with no output and no way to
 * see why. QEMU always emulates a working COM1, so this is invisible there and
 * only bites on real hardware — exactly the kind of hang that is impossible to
 * diagnose in the field.
 *
 * Two parts: a bounded spin (~0.2 s worth of port reads, against the ~87 us a
 * byte actually takes at 115200 — three orders of magnitude of margin), and a
 * STICKY dead flag, because bounding alone would still stall 0.2 s PER BYTE on a
 * dead port, which is a hang by any practical measure. Recovery is automatic and
 * costs one port read: a single probe per call while dead, so a port that starts
 * working (or a later serial_init) resumes logging. Dropping a log byte is
 * strictly better than wedging the kernel. */
#define SERIAL_TX_SPINS 200000u
static volatile int uart_dead;

void serial_putc(char c) {
    if (uart_dead) {
        if (!tx_ready()) return;         /* one cheap probe, then give up again */
        uart_dead = 0;                   /* it came back */
    }
    for (uint32_t i = 0; i < SERIAL_TX_SPINS; i++) {
        if (tx_ready()) {
            outb(COM1 + UART_DATA, (uint8_t)c);
            return;
        }
    }
    uart_dead = 1;                       /* stop paying the timeout on every byte */
}

/* Drain any received bytes into the shared input queue. */
static void serial_handler(struct registers *r) {
    (void)r;
    while (inb(COM1 + UART_LINE_STATUS) & 0x01)   /* data-ready bit */
        input_push((char)inb(COM1 + UART_DATA));
}

void serial_enable_rx_irq(void) {
    outb(COM1 + UART_INT_ENABLE, 0x01);   /* interrupt when data is received */
    irq_install_handler(4, serial_handler);
}
