/*
 * pic.c — remap and drive the 8259 PIC pair.
 *
 * Two cascaded 8259 chips deliver the 16 legacy hardware IRQs. At boot they're
 * configured (from the BIOS's point of view) to raise CPU interrupt vectors
 * 0x08..0x0F and 0x70..0x77. That's a disaster in protected/long mode: vector
 * 0x08 is the CPU's *double fault*. So before enabling interrupts we
 * "remap" the PICs to fire vectors 0x20..0x2F instead, well clear of the
 * 0..31 exception range.
 *
 * Each chip is programmed by writing a fixed sequence of Initialization
 * Command Words (ICW1..ICW4) to its command/data ports.
 */
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11   /* begin init, expect ICW4 */
#define ICW4_8086 0x01   /* 8086/88 mode */
#define PIC_EOI   0x20   /* end-of-interrupt command */

void pic_init(void) {
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();

    outb(PIC1_DATA, PIC_VECTOR_BASE);       io_wait(); /* ICW2: master -> 0x20 */
    outb(PIC2_DATA, PIC_VECTOR_BASE + 8);   io_wait(); /* ICW2: slave  -> 0x28 */

    outb(PIC1_DATA, 0x04); io_wait();  /* ICW3: slave is wired to master IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* ICW3: slave cascade identity = 2    */

    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Mask all IRQs for now; drivers unmask their own line when ready. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);   /* slave first */
    outb(PIC1_CMD, PIC_EOI);       /* master always */
}

void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    outb(port, inb(port) | (1 << bit));
}

void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    outb(port, inb(port) & ~(1 << bit));
}
