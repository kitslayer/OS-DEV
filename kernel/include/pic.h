/* pic.h — the legacy 8259 Programmable Interrupt Controller pair. */
#pragma once
#include <stdint.h>

#define PIC_VECTOR_BASE 0x20    /* IRQ0 -> vector 32, ... IRQ15 -> vector 47 */

void pic_init(void);            /* remap to 0x20/0x28 and mask everything */
void pic_send_eoi(uint8_t irq); /* acknowledge end-of-interrupt */
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
