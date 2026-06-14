/* serial.h — COM1 serial port, our headless debug output. */
#pragma once

void serial_init(void);
void serial_putc(char c);   /* sends one raw byte (no newline translation) */

/* Enable the COM1 receive interrupt (IRQ4); incoming bytes are pushed into the
 * shared input queue, so the serial line can drive the shell. */
void serial_enable_rx_irq(void);
