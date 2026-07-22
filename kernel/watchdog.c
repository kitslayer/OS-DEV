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
#include "io.h"
#include "console.h"

#define TCO_RLD    0x00   /* write to reload (pet) the timer            */
#define TCO1_STS   0x04   /* status 1 (write-1-clear TIMEOUT = bit 3)   */
#define TCO2_STS   0x06   /* status 2 (SECOND_TO_STS = bit 1)           */
#define TCO1_CNT   0x08   /* control: bit 11 = TCO_TMR_HLT (1 = halted) */
#define TCO_TMR    0x12   /* timeout value, bits [9:0], ~0.6 s per tick */

static uint16_t tco_base;      /* TCO I/O base, 0 = no HW watchdog */
static int      wd_enabled;    /* panic-auto-reboot armed */

void watchdog_enable(unsigned secs) {
    (void)secs;
    /* Panic-auto-reboot only (verified, safe): a CPU-exception panic reboots
     * instead of halting, so a crash self-heals via PXE. The Intel TCO HARDWARE
     * watchdog was removed (M1882): it needs an RCBA (PCH root-complex) MMIO write
     * to clear the No-Reboot bit, and doing that blind on the real PCH glitched +
     * hung the machine at framebuffer init — a bad trade for an unverifiable
     * (QEMU wouldn't confirm the reset) feature. Crashes/faults are covered here;
     * a triple-fault self-resets in hardware; a true no-fault hang is the residual
     * gap (a networked smart plug is the clean fix for that). */
    wd_enabled = 1;
    kprintf("[ ok ] panic-auto-reboot armed — a crash self-heals via PXE (no HW watchdog).\n");
}

void watchdog_pet(void) {
    if (tco_base) outw(1, tco_base + TCO_RLD);
}

int watchdog_enabled(void) { return wd_enabled; }
