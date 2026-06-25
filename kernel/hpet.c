/*
 * hpet.c — High Precision Event Timer driver: a real high-resolution
 * clocksource (M1273).
 *
 * The kernel's existing time base is the PIT firing IRQ0 at 100 Hz, so
 * timer_ms() has 10 ms granularity — fine for scheduling, useless for
 * sub-millisecond timing. The HPET is a free-running monotonic up-counter in
 * MMIO whose period is reported (in femtoseconds) by the hardware; reading it
 * gives nanosecond-resolution time with no interrupt and no drift relative to
 * the PIT tick. Every PC since ~2005 has one, and QEMU exposes it by default.
 *
 * Discovery is via the ACPI "HPET" table (acpi_hpet_base()): it hands us the
 * register block's physical base, which we map one page of as uncacheable MMIO
 * (mirroring how smp.c maps the LAPIC at 0xFEE00000 — the HHDM only covers
 * RAM, so MMIO needs an explicit vmm_map with PTE_PCD). We read the capability
 * register for the counter period, enable the main counter, and expose
 * hpet_ns()/hpet_hz()/hpet_counter().
 *
 * SAFE SCOPE: purely additive. We only read the counter and set the global
 * ENABLE_CNF bit (which just lets the counter run — we install no comparators,
 * route no interrupts, and don't touch the PIT/scheduler tick). With no HPET
 * table present hpet_init() is a clean no-op. Mirrors the additive shape of
 * virtio_rng.c.
 */
#include "hpet.h"
#include "acpi.h"
#include "vmm.h"
#include "console.h"

/* HPET register offsets (bytes from the block base; all 64-bit). */
#define HPET_GCAP_ID   0x000   /* General Capabilities & ID: [63:32]=period(fs), [13]=64-bit, [12:8]=#timers-1 */
#define HPET_GEN_CONF  0x010   /* General Configuration: bit0 = ENABLE_CNF (run the main counter) */
#define HPET_MAIN_CNT  0x0F0   /* Main Counter Value (the free-running up-counter) */

static volatile uint64_t *hpet;        /* mapped register block, or 0 if absent */
static uint64_t hpet_period_fs;        /* counter tick period, femtoseconds (from the cap register) */
static uint64_t hpet_freq_hz;          /* derived counter frequency = 1e15 / period_fs */

static inline uint64_t rd(uint32_t off) { return hpet[off / 8]; }
static inline void     wr(uint32_t off, uint64_t v) { hpet[off / 8] = v; }

void hpet_init(void) {
    uint64_t base = acpi_hpet_base();
    if (!base) { kprintf("[hpet] no HPET table (high-res clock unavailable; PIT tick still used).\n"); return; }

    /* Map the register block as uncacheable MMIO at its HHDM address (the HHDM
     * only direct-maps RAM, so this MMIO frame needs an explicit mapping — the
     * exact pattern smp.c uses for the LAPIC). One page covers the 1 KiB block. */
    volatile uint64_t *regs = (volatile uint64_t *)hhdm(base & ~0xFFFull);
    vmm_map((uint64_t)regs, base & ~0xFFFull, PTE_WRITABLE | PTE_PCD);
    hpet = (volatile uint64_t *)((uint8_t *)regs + (base & 0xFFF));

    uint64_t cap = rd(HPET_GCAP_ID);
    hpet_period_fs = cap >> 32;                       /* COUNTER_CLK_PERIOD, femtoseconds */
    /* Sanity: the spec caps the period at 100 ns (1e8 fs); a zero/insane value
     * means we mis-read the block, so disable rather than divide by it. */
    if (hpet_period_fs == 0 || hpet_period_fs > 100000000ull) {
        kprintf("[hpet] bad counter period %lu fs — disabling.\n", hpet_period_fs);
        hpet = 0; hpet_period_fs = 0; return;
    }
    hpet_freq_hz = 1000000000000000ull / hpet_period_fs;   /* 1e15 fs/s / period = Hz */

    wr(HPET_GEN_CONF, rd(HPET_GEN_CONF) | 1ull);      /* ENABLE_CNF: start the main counter */
    kprintf("[ ok ] HPET: %u-bit counter @ %lu Hz (period %lu fs), main counter enabled\n",
            (cap & (1u << 13)) ? 64 : 32, hpet_freq_hz, hpet_period_fs);
}

int      hpet_present(void) { return hpet != 0; }
uint64_t hpet_hz(void)      { return hpet_freq_hz; }
uint64_t hpet_counter(void) { return hpet ? rd(HPET_MAIN_CNT) : 0; }

/* Nanoseconds since the counter started. counter * period_fs / 1e6 (fs->ns).
 * The product stays within 64 bits for ~30 min of uptime at 100 MHz, which is
 * ample for the high-res deltas this is used for; a 128-bit widen is a
 * follow-on if a long-running absolute clock is ever needed. */
uint64_t hpet_ns(void) { return hpet ? hpet_counter() * hpet_period_fs / 1000000ull : 0; }
