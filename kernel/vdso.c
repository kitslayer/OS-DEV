/*
 * vdso.c — the vDSO time page (M1111).
 *
 * One physical frame, mapped read-only into every userspace address space at
 * VDSO_ADDR. The timer IRQ writes the current monotonic + wall-clock time into
 * it under a seqlock; userspace reads it directly (see ulib clock_gettime) with
 * zero syscalls — the classic Linux vDSO trick for gettimeofday-class calls.
 *
 * Frame lifetime: the page is shared by all processes, so it must never be
 * freed when one exits. We hold a *permanent* reference (one pmm_addref at init)
 * and take one more ref per mapping; an address space's teardown frees the leaf
 * once (decrementing), so the refcount never reaches the single-owner state that
 * pmm_free_frame would actually release. Same pattern as the M1089 ring mirror.
 */
#include "vdso.h"
#include "vmm.h"        /* hhdm(), vmm_map_to(), PTE_* */
#include "pmm.h"        /* PAGE_SIZE, pmm_alloc_frame(), pmm_addref() */
#include "timer.h"      /* timer_ticks() */
#include "rtc.h"        /* rtc_now() — seed the wall clock from CMOS */

static uint64_t          g_phys;        /* physical frame backing the page (0 = not initialised) */
static struct vdso_time *g_vt;          /* kernel HHDM view, for writing */
static uint64_t          g_base_unix;   /* Unix seconds at g_base_ticks */
static uint64_t          g_base_ticks;  /* tick count when the wall clock was last (re)based */

/* Days since the Unix epoch for a proleptic-Gregorian y/m(1-12)/d — Howard
 * Hinnant's days_from_civil (the inverse of the unix_to_rtc in net.c). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);                 /* [0, 399] */
    uint64_t doy = (153 * (uint64_t)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (uint64_t)d - 1;  /* [0, 365] */
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

static uint64_t rtc_to_unix(void) {
    struct rtc_time t; rtc_now(&t);
    int64_t days = days_from_civil(t.year, t.month, t.day);
    return (uint64_t)(days * 86400 + t.hour * 3600 + t.min * 60 + t.sec);
}

void vdso_init(void) {
    g_phys = pmm_alloc_frame();
    if (!g_phys) return;                 /* OOM at boot — clock_gettime falls back nowhere, but this won't happen */
    pmm_addref(g_phys);                  /* permanent reference: survives every app teardown */
    g_vt = (struct vdso_time *)hhdm(g_phys);
    for (uint64_t i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) ((volatile uint64_t *)g_vt)[i] = 0;
    g_vt->hz     = 100;                  /* matches timer_init(100) */
    g_base_ticks = timer_ticks();
    g_base_unix  = rtc_to_unix();
    vdso_tick(g_base_ticks);
}

void vdso_map(uint64_t cr3) {
    if (!g_phys) return;
    /* Read-only (no PTE_WRITABLE), user-readable, non-executable. */
    if (vmm_map_to(cr3, VDSO_ADDR, g_phys, PTE_USER | PTE_NX) == 0)
        pmm_addref(g_phys);              /* one ref per live mapping */
}

void vdso_set_realtime(uint64_t unix_sec) {
    g_base_unix  = unix_sec;
    g_base_ticks = timer_ticks();
}

void vdso_tick(uint64_t ticks) {
    if (!g_vt) return;                   /* before vdso_init (early-boot IRQs): no-op */
    uint32_t hz = g_vt->hz ? g_vt->hz : 100;
    uint64_t ns_per_tick = 1000000000ull / hz;
    uint64_t dt = ticks - g_base_ticks;

    g_vt->seq++;                         /* -> odd: update in progress */
    __asm__ volatile("" ::: "memory");
    g_vt->ticks     = ticks;
    g_vt->mono_ns   = ticks * ns_per_tick;
    g_vt->real_sec  = g_base_unix + dt / hz;
    g_vt->real_nsec = (uint32_t)((dt % hz) * ns_per_tick);
    __asm__ volatile("" ::: "memory");
    g_vt->seq++;                         /* -> even: snapshot complete */
}
