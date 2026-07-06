/* timer.h — the 8253/8254 Programmable Interval Timer (PIT) on IRQ0. */
#pragma once
#include <stdint.h>

void     timer_init(uint32_t hz);   /* program the PIT and install IRQ0 handler */
uint64_t timer_ticks(void);         /* ticks since boot */
uint64_t timer_ms(void);            /* milliseconds since boot (monotonic) */
void     timer_wait(uint64_t ticks);/* busy-sleep this many ticks (hlt-based) */
uint32_t timer_tick_ms(void);       /* ms per tick (1000/hz); APs use this to charge CPU time at the same rate (M1548) */
