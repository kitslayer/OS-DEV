/*
 * watchdog.c — Intel TCO hardware watchdog + panic-auto-reboot (M1881).
 *
 * For the autonomous real-hardware bring-up loop: a diskless machine PXE-boots
 * the latest kernel, and I drive it over netcon. But `netcon reboot` only works
 * while the kernel is alive — a hang or crash locks me out. This closes that gap:
 *
 *   - Hardware watchdog (Intel TCO, in the PCH's LPC block): a free-running timer
 *     that RESETS the machine if the kernel stops "petting" it. We pet from the
 *     PIT IRQ, so the reset fires exactly when the system is truly wedged (the
 *     timer/scheduler stopped) — not merely when one task spins (preemption keeps
 *     the tick alive). After the reset it PXE-boots the latest kernel → self-heal.
 *   - panic-auto-reboot: a CPU-exception panic (the common failure when I ship a
 *     bad change) normally halts forever; with the watchdog enabled it instead
 *     reboots after a short delay, so faults self-heal too (and faster than the
 *     TCO timeout).
 *
 * Enabled only via the `watchdog` cmdline flag (the PXE bring-up image sets it),
 * so `make check`'s deliberate-fault tests still halt for inspection.
 *
 * TCO details: the timer lives at ACPI PMBASE + 0x60, and PMBASE is in the PCH
 * LPC bridge's PCI config (00:1f.0, register 0x40). Lynx Point (the bring-up
 * laptop's PCH) is TCO v2: 16-bit TCO_TMR at 0x12, ~0.6 s/tick. NOTE: if the
 * firmware set the RCBA "No-Reboot" bit, the second timeout won't reboot — then
 * only panic-auto-reboot + a triple-fault reset apply (still covers the common
 * cases); clearing that bit needs RCBA access, a follow-up if it proves necessary.
 */
#include "watchdog.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "console.h"

#define TCO_RLD    0x00   /* write to reload (pet) the timer            */
#define TCO1_STS   0x04   /* status 1 (write-1-clear TIMEOUT = bit 3)   */
#define TCO2_STS   0x06   /* status 2 (SECOND_TO_STS = bit 1)           */
#define TCO1_CNT   0x08   /* control: bit 11 = TCO_TMR_HLT (1 = halted) */
#define TCO_TMR    0x12   /* timeout value, bits [9:0], ~0.6 s per tick */

static uint16_t tco_base;      /* TCO I/O base, 0 = no HW watchdog */
static int      wd_enabled;    /* panic-auto-reboot armed */

void watchdog_enable(unsigned secs) {
    wd_enabled = 1;            /* panic-auto-reboot on, even if no TCO is found */

    pci_device_t lpc = pci_find_class(0x06, 0x01, 0x00);   /* ISA bridge (prog_if 0) = the PCH LPC */
    if (!lpc.valid || lpc.vendor_id != 0x8086) {
        kprintf("[watchdog] no Intel PCH LPC — HW watchdog off; panic-auto-reboot on.\n");
        return;
    }
    /* PIIX3/PIIX4 (QEMU's default i440fx south bridge) have no TCO watchdog — the
     * TCO block is ICH6+/PCH. Skip so we don't poke unrelated PM registers there;
     * q35 (ICH9) and real PCHs do have it. */
    if (lpc.device_id == 0x7000 || lpc.device_id == 0x7110) {
        kprintf("[watchdog] LPC is PIIX (no TCO) — HW watchdog off; panic-auto-reboot on.\n");
        return;
    }
    uint16_t pmbase = pci_read16(lpc.bus, lpc.slot, lpc.func, 0x40) & 0xFF80;
    if (!pmbase) { kprintf("[watchdog] no ACPI PMBASE — HW watchdog off; panic-auto-reboot on.\n"); return; }
    tco_base = pmbase + 0x60;

    /* Clear the "No-Reboot" bit so the TCO's second timeout actually RESETS the
     * machine (the firmware often leaves it set). It lives in RCBA + 0x3410 (GCS)
     * bit 5; RCBA base is LPC config 0xF0 (bits 31:14, bit 0 = enable). We map
     * that MMIO page and read-modify-write only bit 5. (M1881) */
    uint32_t rcba = pci_read32(lpc.bus, lpc.slot, lpc.func, 0xF0);
    if (rcba & 1) {
        uint64_t gcs_pa = (rcba & 0xFFFFC000u) + 0x3410;
        uint64_t pg = gcs_pa & ~0xFFFull;
        volatile uint32_t *gcs = (volatile uint32_t *)((uint8_t *)hhdm(pg) + (gcs_pa & 0xFFF));
        vmm_map((uint64_t)(uintptr_t)hhdm(pg), pg, PTE_WRITABLE | PTE_PCD);
        *gcs = *gcs & ~(1u << 5);          /* No-Reboot = 0 -> TCO reset enabled */
    }

    uint16_t ticks = (uint16_t)((secs * 10u) / 6u);        /* ~0.6 s per tick */
    if (ticks < 4)     ticks = 4;
    if (ticks > 0x3FF) ticks = 0x3FF;

    outw(1u << 3, tco_base + TCO1_STS);                    /* clear a stale TIMEOUT status  */
    outw(1u << 1, tco_base + TCO2_STS);                    /* clear a stale SECOND_TO status */
    uint16_t tmr = (inw(tco_base + TCO_TMR) & ~0x3FFu) | ticks;
    outw(tmr, tco_base + TCO_TMR);
    outw(1, tco_base + TCO_RLD);                           /* load the new timeout */
    uint16_t cnt = inw(tco_base + TCO1_CNT) & ~(1u << 11); /* clear TCO_TMR_HLT -> run */
    outw(cnt, tco_base + TCO1_CNT);
    outw(1, tco_base + TCO_RLD);                           /* first pet */

    kprintf("[ ok ] TCO hardware watchdog armed (~%us, TCOBASE 0x%x) — a hang self-resets into PXE.\n",
            secs, tco_base);
}

void watchdog_pet(void) {
    if (tco_base) outw(1, tco_base + TCO_RLD);
}

int watchdog_enabled(void) { return wd_enabled; }
