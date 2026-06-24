/* idt.h — Interrupt Descriptor Table. */
#pragma once

void idt_init(void);   /* build the 256-entry IDT and lidt it */
void idt_load(void);   /* lidt the (BSP-built) IDT on this CPU — for APs (M1198) */
