/* timer.h — the 8253/8254 Programmable Interval Timer (PIT) on IRQ0. */
#pragma once
#include <stdint.h>

void     timer_init(uint32_t hz);   /* program the PIT and install IRQ0 handler */
uint64_t timer_ticks(void);         /* ticks since boot */
void     timer_wait(uint64_t ticks);/* busy-sleep this many ticks (hlt-based) */
