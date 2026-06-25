/* hpet.h — High Precision Event Timer: a high-resolution monotonic clocksource
 * discovered via the ACPI HPET table (M1273). Purely additive: no-op if absent. */
#pragma once
#include <stdint.h>

void     hpet_init(void);     /* discover via ACPI, map MMIO, enable the main counter (logs what it found) */
int      hpet_present(void);  /* 1 if an HPET was found and enabled */
uint64_t hpet_hz(void);       /* counter frequency in Hz (0 if absent) */
uint64_t hpet_counter(void);  /* raw main-counter value (0 if absent) */
uint64_t hpet_ns(void);       /* nanoseconds since the counter started (0 if absent) */
