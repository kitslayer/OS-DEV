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
#include "task.h"
#include "timer.h"      /* timer_tick_ms — per-core CPU-time accounting (M1548) */
#include "vmm.h"        /* kstack_is_guard — flag a kernel-stack-overflow #PF (M1495) */
#include "ksyms.h"
#include "ioapic.h"     /* I/O APIC routing — move a live IRQ off the PIC (M1857) */
#include "acpi.h"       /* acpi_madt_gsi_for_irq — ISA IRQ -> GSI (M1857) */
#include "smp.h"        /* lapic_eoi — ack an I/O APIC-delivered IRQ (M1857) */
#include "smp.h"
#include "watchdog.h"
#include "gdbstub.h"
#include "msi.h"          /* MSI vector block + msi_install_handler/msi_irq_count */
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

/* MSI/MSI-X vectors (M1288): a parallel registry over the reserved MSI vector
 * block. Populated by kernel/msi.c (msi_alloc_vector -> msi_install_handler) and
 * fired from isr_dispatch's MSI branch below. */
static irq_handler_fn msi_handlers[MSI_VEC_COUNT];
static uint64_t       msi_counts[MSI_VEC_COUNT];

void msi_install_handler(uint8_t vector, irq_handler_fn fn) {
    if (vector >= MSI_VEC_BASE && vector < MSI_VEC_BASE + MSI_VEC_COUNT)
        msi_handlers[vector - MSI_VEC_BASE] = fn;
}
uint64_t msi_irq_count(uint8_t vector) {
    return (vector >= MSI_VEC_BASE && vector < MSI_VEC_BASE + MSI_VEC_COUNT)
         ? msi_counts[vector - MSI_VEC_BASE] : 0;
}

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

/* IRQs whose delivery has been moved from the 8259 PIC to the I/O APIC (M1857):
 * the same handler runs (same vector 32+irq), but the dispatch must acknowledge
 * via the LOCAL APIC, not the PIC. */
static uint32_t g_ioapic_routed;

/* Route ISA `irq`'s delivery through the I/O APIC (to vector 32+irq on the BSP's
 * local APIC), masking it on the PIC. No-op if no I/O APIC. The handler is
 * unchanged; only the delivery path + EOI target change. */
void irq_route_ioapic(uint8_t irq) {
    if (irq >= 16 || !ioapic_present()) return;
    uint32_t gsi = acpi_madt_gsi_for_irq(irq);
    ioapic_route((uint8_t)gsi, (uint8_t)(32 + irq), 0 /* BSP APIC id (0 on QEMU) */, 0 /* unmasked */);
    pic_mask(irq);                                   /* the PIC no longer delivers this line */
    g_ioapic_routed |= (1u << irq);
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
    /* Remember the most recent ring-3 trap frame for /proc/<pid>/regs (M1119):
     * while a task is stopped, this stays valid (frozen on its kernel stack). */
    if ((r->cs & 3) == 3) { task_t *ct = task_self(); if (ct) ct->uframe = r; }

    if (r->int_no == SYSCALL_VECTOR) {     /* int 0x80 from userspace */
        syscall_dispatch(r);
        return;
    }

    /* SMP inter-processor interrupts (M1198): an AP takes the wake IPI to break
     * out of its idle hlt (the work-drain happens in its idle loop after iret);
     * the LAPIC spurious vector needs no EOI. Both just acknowledge + return. */
    if (r->int_no == 0x40) { lapic_eoi(); return; }
    /* This core's own local LAPIC timer (M1532): the real per-core preemption
     * source, armed by lapic_timer_start_this_cpu() on every AP (never the
     * BSP, which keeps its own working PIT-driven tick_handler). EOI FIRST,
     * exactly like the IRQ 32-47 branch below and for the identical reason:
     * sched_tick() can context_switch away and not return here until this
     * exact task/core is picked again, so the EOI must already be sent or the
     * LAPIC would never deliver this vector again — an earlier version of
     * this same mechanism (M1531's broadcast-IPI predecessor to this) hit
     * exactly that as a real in-guest crash (an interrupt pileup deep enough
     * to double-fault) before the order was fixed; kept EOI-first here from
     * the start. */
    if (r->int_no == 0x42) {
        lapic_eoi();
        /* Charge THIS core's own current task, same as timer_handler does for
         * the BSP -- task_cpu_tick/app_alarm_tick/app_cpulimit_tick all key off
         * task_self()/current (whichever task is running on the core executing
         * this code right now), so calling them here is correct, not a double-
         * count. Found via a real, reproducible bug: utime_ms/stime_ms stayed
         * 0 forever for a task that happened not to be BSP-resident, because
         * task_cpu_tick was ONLY ever invoked from timer.c's PIT handler --
         * legacy IRQ0 has exactly one target, so that path is BSP-exclusive by
         * construction (M1548). Deliberately NOT adding app_timer_tick() or
         * anything else from timer_handler here: those scan ALL apps globally
         * per call, so running them once per core per tick would fire them
         * (cores) times too often instead of once. */
        task_cpu_tick(timer_tick_ms(), (r->cs & 3) == 3);
        app_alarm_tick();
        app_cpulimit_tick();
        sched_tick();
        return;
    }
    if (r->int_no == 0xFF) { return; }

    /* MSI / MSI-X message-signaled interrupts (M1288): a device wrote its
     * configured vector to the LAPIC. Run the registered handler (if any), tally
     * the delivery, and acknowledge the LOCAL APIC — MSI bypasses the PIC. */
    if (r->int_no >= MSI_VEC_BASE && r->int_no < MSI_VEC_BASE + MSI_VEC_COUNT) {
        int idx = (int)r->int_no - MSI_VEC_BASE;
        msi_counts[idx]++;
        if (msi_handlers[idx]) msi_handlers[idx](r);
        lapic_eoi();
        return;
    }

    if (r->int_no < 32) {
        /* Breakpoint is recoverable — report and continue past the int3. */
        if (r->int_no == 3) {
            if (gdbstub_armed()) { gdbstub_serve(r); return; }   /* GDB stub: hand gdb the trap frame, resume on continue/detach (M1204) */
            kprintf("[int] #BP breakpoint trap at rip=%p (resuming)\n",
                    (void *)r->rip);
            return;
        }

        /* #DB single-step trap from ring 3 (M1123): record the instruction and
         * either keep stepping or stop. Never kills (a #DB just means we stepped
         * one instruction); app_singlestep_trap clears TF if it's not tracing. */
        if (r->int_no == 1) {
            /* gdb single-step lands here as a RING-0 #DB (the kernel was stepped);
             * clear TF and re-enter the stub so gdb sees the new state (M1205). */
            if ((r->cs & 3) == 0 && gdbstub_armed()) { r->rflags &= ~(1ull << 8); gdbstub_serve(r); return; }
            if ((r->cs & 3) == 3) { app_singlestep_trap(r); return; }
        }

        /* A fault from ring 3 (CS RPL == 3) is a userspace app bug, not a kernel
         * bug: report it and terminate just that task, leaving the kernel and the
         * rest of the desktop running. (A ring-0 fault falls through and panics —
         * that IS a real kernel bug.) */
        if ((r->cs & 3) == 3) {
            uint64_t cr2 = 0;
            if (r->int_no == 14) {                 /* page fault: maybe a demand-paged mmap region */
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                if (app_fault_handle(cr2, r->err_code)) return;  /* COW copy / swap-in / mapped a reserved page -> retry */
            }
            if (app_signal_deliver(r, 11)) return;  /* SIGSEGV: a registered handler catches the fault */
            kprintf("[fault] %s (vector %lu) err=0x%lx in a ring-3 task at rip=%p (CR2=%p) -- terminating it\n",
                    exception_names[r->int_no], r->int_no, r->err_code, (void *)r->rip, (void *)cr2);
            app_fault_current(r);  /* dump a core, mark the app exited + task_exit(); does not return */
        }

        interrupts_disable();
        kprintf("\n*** KERNEL PANIC: CPU EXCEPTION ***\n");
        kprintf("  %s (vector %lu)", exception_names[r->int_no], r->int_no);
        kprintf("   error_code=0x%lx\n", r->err_code);
        if (r->int_no == 14) {           /* page fault: CR2 = faulting address */
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            kprintf("  faulting address (CR2) = %p\n", (void *)cr2);
            if (kstack_is_guard(cr2))    /* CR2 in the guarded-stack window -> a task overran its kernel stack into a guard page (M1495) */
                kprintf("  >>> KERNEL STACK OVERFLOW: a task overran its kernel stack into a guard page <<<\n");
        }
        /* A stack overflow usually surfaces as a #DF, not a clean #PF: once RSP is in
         * the guard page, the CPU can't push the #PF's own exception frame, so it
         * escalates to a double fault (taken on IST1). Diagnose that too (M1498). */
        if (r->int_no == 8 && kstack_is_guard(r->rsp))
            kprintf("  >>> KERNEL STACK OVERFLOW: a task overran its kernel stack (its #PF escalated to a #DF) <<<\n");
        dump_registers(r);
        backtrace(r->rip, r->rbp);       /* symbolized call trace (kernel/ksyms.c) */
        /* Autonomous bring-up (M1881): with the watchdog enabled, a panic REBOOTS
         * instead of halting forever, so a bad change self-heals — the machine
         * PXE-boots the latest kernel. The brief spin leaves the panic on the
         * framebuffer/serial first. (Off by default: `make check`'s deliberate
         * faults still halt for inspection.) */
        if (watchdog_enabled()) {
            kprintf("  watchdog: rebooting in ~10s...\n");
            for (volatile uint64_t d = 0; d < 8000000000ULL; d++) { }
            acpi_reboot();
        }
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
        if (g_ioapic_routed & (1u << irq)) lapic_eoi();   /* I/O APIC-delivered: ack the LOCAL APIC (M1857) */
        else                               pic_send_eoi(irq);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        /* On the way back to ring 3 (after the timer may have rescheduled us),
         * deliver any async signal pending on the resuming app — this is what
         * lets Ctrl-C interrupt a runaway ring-3 compute loop. No-op unless an
         * app opted in with a SIGINT handler and has one pending. (M1083) */
        app_deliver_pending(r);
    }
}
