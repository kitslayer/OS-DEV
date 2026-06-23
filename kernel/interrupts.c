/*
 * interrupts.c — the C side of interrupt handling.
 *
 * Every interrupt funnels through isr_dispatch() with the saved CPU state.
 * Two cases:
 *
 *   vector  0..31  -> a CPU *exception*. Something the running code did (or
 *                     the demo `int3`). Fatal ones print a register dump and
 *                     halt; breakpoint (#BP) we just report and resume.
 *   vector 32..47  -> a hardware *IRQ* from the PIC. Call the registered
 *                     driver handler (if any), then acknowledge with an EOI.
 */
#define __KERNEL__
#include "interrupts.h"
#include "idt.h"
#include "pic.h"
#include "console.h"
#include "syscall.h"
#include "app.h"
#include "ksyms.h"
#include <stdint.h>

static const char *const exception_names[32] = {
    "Divide Error", "Debug", "Non-Maskable Interrupt", "Breakpoint",
    "Overflow", "BOUND Range Exceeded", "Invalid Opcode",
    "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
    "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point", "Alignment Check", "Machine Check",
    "SIMD Floating-Point", "Virtualization", "Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security", "Reserved",
};

static irq_handler_fn irq_handlers[16];
static uint64_t irq_counts[16];                  /* IRQ fire tally (for /proc/interrupts) */
uint64_t irq_count(int i) { return (i >= 0 && i < 16) ? irq_counts[i] : 0; }

void interrupts_init(void) {
    idt_init();
    pic_init();
}

void interrupts_enable(void)  { __asm__ volatile("sti"); }
void interrupts_disable(void) { __asm__ volatile("cli"); }

void irq_install_handler(uint8_t irq, irq_handler_fn fn) {
    if (irq < 16) {
        irq_handlers[irq] = fn;
        pic_unmask(irq);
    }
}

static void dump_registers(struct registers *r) {
    kprintf("  rax=%016lx rbx=%016lx rcx=%016lx\n", r->rax, r->rbx, r->rcx);
    kprintf("  rdx=%016lx rsi=%016lx rdi=%016lx\n", r->rdx, r->rsi, r->rdi);
    kprintf("  rbp=%016lx rsp=%016lx rip=%016lx\n", r->rbp, r->rsp, r->rip);
    kprintf("  r8 =%016lx r9 =%016lx r10=%016lx\n", r->r8,  r->r9,  r->r10);
    kprintf("  r11=%016lx r12=%016lx r13=%016lx\n", r->r11, r->r12, r->r13);
    kprintf("  r14=%016lx r15=%016lx\n",            r->r14, r->r15);
    kprintf("  cs=%lx ss=%lx rflags=%016lx\n",      r->cs,  r->ss,  r->rflags);
}

void isr_dispatch(struct registers *r) {
    if (r->int_no == SYSCALL_VECTOR) {     /* int 0x80 from userspace */
        syscall_dispatch(r);
        return;
    }

    if (r->int_no < 32) {
        /* Breakpoint is recoverable — report and continue past the int3. */
        if (r->int_no == 3) {
            kprintf("[int] #BP breakpoint trap at rip=%p (resuming)\n",
                    (void *)r->rip);
            return;
        }

        /* A fault from ring 3 (CS RPL == 3) is a userspace app bug, not a kernel
         * bug: report it and terminate just that task, leaving the kernel and the
         * rest of the desktop running. (A ring-0 fault falls through and panics —
         * that IS a real kernel bug.) */
        if ((r->cs & 3) == 3) {
            if (r->int_no == 14) {                 /* page fault: maybe a demand-paged mmap region */
                uint64_t cr2;
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                if (app_fault_handle(cr2)) return;  /* mapped a reserved page -> retry the instruction */
            }
            if (app_signal_deliver(r, 11)) return;  /* SIGSEGV: a registered handler catches the fault */
            kprintf("[fault] %s (vector %lu) err=0x%lx in a ring-3 task at rip=%p -- terminating it\n",
                    exception_names[r->int_no], r->int_no, r->err_code, (void *)r->rip);
            app_fault_current();   /* marks the app exited + task_exit(); does not return */
        }

        interrupts_disable();
        kprintf("\n*** KERNEL PANIC: CPU EXCEPTION ***\n");
        kprintf("  %s (vector %lu)", exception_names[r->int_no], r->int_no);
        kprintf("   error_code=0x%lx\n", r->err_code);
        if (r->int_no == 14) {           /* page fault: CR2 = faulting address */
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            kprintf("  faulting address (CR2) = %p\n", (void *)cr2);
        }
        dump_registers(r);
        backtrace(r->rip, r->rbp);       /* symbolized call trace (kernel/ksyms.c) */
        kprintf("  system halted.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    if (r->int_no >= 32 && r->int_no < 48) {
        uint8_t irq = (uint8_t)(r->int_no - 32);
        irq_counts[irq]++;                       /* per-IRQ tally for /proc/interrupts */
        /* Acknowledge FIRST, then run the handler. This matters for preemption:
         * the timer handler context-switches away and won't return here until
         * this task runs again, so the EOI must already be sent or the PIC
         * would never deliver another tick. Safe because interrupt gates keep
         * IF clear during the handler (no re-entry). */
        pic_send_eoi(irq);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
    }
}
