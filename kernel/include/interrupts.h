/*
 * interrupts.h — the saved-state struct and the IRQ handler API.
 *
 * When any interrupt fires, a tiny assembly stub saves the full CPU state onto
 * the stack in a fixed order, then hands C a pointer to it as `struct
 * registers`. The field order here MUST match the push order in
 * isr_stubs.asm exactly — the first field is what ends up at the lowest
 * address (the last thing pushed).
 */
#pragma once
#include <stdint.h>

struct registers {
    /* pushed by isr_common, in reverse (r15 last == lowest address) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* pushed by the per-vector stub */
    uint64_t int_no, err_code;
    /* pushed by the CPU on interrupt entry */
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_fn)(struct registers *regs);

uint64_t irq_count(int i);                  /* times IRQ i has fired (for /proc/interrupts) */
void interrupts_init(void);                 /* load IDT + remap the PIC */
void interrupts_enable(void);               /* sti */
void interrupts_disable(void);              /* cli */

/* Register a handler for hardware IRQ 0..15 and unmask it on the PIC. */
void irq_install_handler(uint8_t irq, irq_handler_fn fn);

/* Called from assembly (isr_common). Not for general use. */
void isr_dispatch(struct registers *regs);
