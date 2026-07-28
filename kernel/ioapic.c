/*
 * ioapic.c — I/O APIC (interrupt routing) driver (M1856).
 *
 * The OS has always run interrupts through the legacy 8259 PIC (4 lines:
 * PIT/keyboard/serial/mouse) and POLLED everything else. The I/O APIC is the
 * modern replacement: it takes the 24 Global System Interrupts (GSIs) and
 * routes each, via a per-GSI "redirection entry", to a chosen interrupt vector
 * delivered to a chosen local APIC (CPU). Programming it is the prerequisite for
 * interrupt-driven device I/O (so a NIC/disk can raise an IRQ on completion
 * instead of the CPU spin-polling).
 *
 * History: M1856 landed this file inert — it located + mapped the I/O APIC and
 * exposed the routing primitives with every redirection entry MASKED, so the
 * live IRQ path stayed 100% on the 8259 PIC. M1857 moved one line (the
 * keyboard) across to prove the LAPIC-EOI delivery path end to end. M1890
 * finished the job: the PIT tick, keyboard, serial, PS/2 mouse and the NIC's
 * PCI line are all delivered here now, and ioapic_route_ex() takes the
 * electrical configuration (polarity + trigger mode) rather than assuming the
 * ISA default of edge-triggered/active-high — which is what made routing a PCI
 * INTx line (level-triggered, active-LOW) possible at all.
 *
 * MMIO: two registers at the base — IOREGSEL (offset 0x00) selects a register
 * index, IOWIN (offset 0x10) reads/writes it. Redirection entry n is the 64-bit
 * register pair 0x10+2n (low) / 0x11+2n (high).
 */
#include "ioapic.h"
#include "acpi.h"
#include "vmm.h"       /* hhdm() */
#include "console.h"
#include <stdint.h>

static volatile uint32_t *g_io;     /* mapped MMIO window (dword-indexed) */
static uint32_t g_gsi_base;         /* first GSI this I/O APIC handles */
static uint32_t g_nredir;           /* number of redirection entries */

/* IOREGSEL is dword 0, IOWIN is at byte offset 0x10 = dword 4. */
static uint32_t io_read(uint32_t reg)  { g_io[0] = reg; return g_io[4]; }
static void     io_write(uint32_t reg, uint32_t val) { g_io[0] = reg; g_io[4] = val; }

int ioapic_present(void) { return g_io != 0; }
uint32_t ioapic_gsi_base(void) { return g_gsi_base; }
uint32_t ioapic_num_redir(void) { return g_nredir; }

/* Program redirection entry for `gsi`: deliver `vector` (fixed delivery,
 * physical destination) to local APIC `apic_id`, (un)masked, with an explicit
 * electrical configuration.
 *
 * The low dword's layout (Intel ICH/IOAPIC §): [7:0] vector, [10:8] delivery
 * mode (000 = fixed), [11] destination mode (0 = physical), [12] delivery
 * status (RO), [13] polarity (0 = active high, 1 = active LOW), [14] remote IRR
 * (RO, level only), [15] trigger mode (0 = edge, 1 = LEVEL), [16] mask.
 *
 * `active_low` and `level` were previously hardcoded to 0 — correct for a plain
 * ISA line, and wrong for anything else. PCI interrupt lines are level-triggered
 * and active-low by definition, so routing one with the old edge/active-high
 * entry either never fires or fires continuously; and an ISA line the firmware
 * remapped (per the MADT override flags) may be either. (M1890) */
void ioapic_route_ex(uint8_t gsi, uint8_t vector, uint8_t apic_id, int masked,
                     int active_low, int level) {
    if (!g_io || gsi < g_gsi_base) return;
    uint32_t idx = gsi - g_gsi_base;
    if (idx >= g_nredir) return;
    uint32_t lo = (uint32_t)vector
                | (active_low ? (1u << 13) : 0)
                | (level      ? (1u << 15) : 0)
                | (masked     ? (1u << 16) : 0);
    uint32_t hi = (uint32_t)apic_id << 24;                        /* [63:56]=dest APIC id */
    /* Mask the entry before rewriting it, so a line that is already live can't
     * deliver against a half-updated (vector-changed, destination-stale) entry. */
    io_write(0x10 + 2 * idx, io_read(0x10 + 2 * idx) | (1u << 16));
    io_write(0x10 + 2 * idx + 1, hi);                             /* high first, then low arms it */
    io_write(0x10 + 2 * idx, lo);
}

/* Back-compatible edge-triggered / active-high routing (the ISA default). */
void ioapic_route(uint8_t gsi, uint8_t vector, uint8_t apic_id, int masked) {
    ioapic_route_ex(gsi, vector, apic_id, masked, 0, 0);
}
void ioapic_mask(uint8_t gsi) {
    if (!g_io || gsi < g_gsi_base) return;
    uint32_t idx = gsi - g_gsi_base; if (idx >= g_nredir) return;
    io_write(0x10 + 2 * idx, io_read(0x10 + 2 * idx) | (1u << 16));
}
void ioapic_unmask(uint8_t gsi) {
    if (!g_io || gsi < g_gsi_base) return;
    uint32_t idx = gsi - g_gsi_base; if (idx >= g_nredir) return;
    io_write(0x10 + 2 * idx, io_read(0x10 + 2 * idx) & ~(1u << 16));
}

void ioapic_init(void) {
    uint32_t base = 0, gsi = 0;
    if (!acpi_madt_ioapic(&base, &gsi)) {
        kprintf("[ioapic] no I/O APIC in the ACPI MADT (interrupts stay on the 8259 PIC).\n\n");
        return;
    }
    g_io = (volatile uint32_t *)hhdm(base);
    vmm_map((uint64_t)g_io, base, PTE_WRITABLE | PTE_PCD);   /* map the MMIO page, cache-disabled (like the LAPIC) */
    g_gsi_base = gsi;
    uint32_t ver = io_read(0x01);                        /* IOAPICVER: [23:16] = max redir entry */
    g_nredir = ((ver >> 16) & 0xFF) + 1;
    if (g_nredir == 0 || g_nredir > 240) g_nredir = 24;  /* sane fallback */

    /* Leave every entry MASKED (the PIC still owns live IRQs). */
    for (uint32_t i = 0; i < g_nredir; i++) {
        io_write(0x10 + 2 * i + 1, 0);
        io_write(0x10 + 2 * i, 1u << 16);                /* masked */
    }

    /* Self-check: program the first entry (masked) and read it back. */
    ioapic_route((uint8_t)gsi, 0x20, 0, 1);
    uint32_t rb = io_read(0x10);
    int ok = ((rb & 0xFF) == 0x20) && (rb & (1u << 16));
    ioapic_mask((uint8_t)gsi);

    kprintf("[ %s ] I/O APIC at 0x%x: GSI base %u, %u redirection entries (ver 0x%x)%s\n\n",
            ok ? "ok" : "!!", (unsigned)base, (unsigned)gsi, (unsigned)g_nredir,
            (unsigned)(ver & 0xFF), ok ? " — routing ready (entries masked until each IRQ is routed over)" : " READBACK FAILED");
}
