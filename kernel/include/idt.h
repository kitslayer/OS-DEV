/* idt.h — Interrupt Descriptor Table. */
#pragma once

void idt_init(void);   /* build the 256-entry IDT and lidt it */
