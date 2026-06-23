/*
 * profile.h — a statistical (sampling) kernel profiler (M1086).
 *
 * While enabled, each timer tick records the RIP the timer interrupted IF it
 * was kernel-mode code, aggregating samples per function (via the kallsyms
 * symbolizer). `/proc/profile` reads back the hot functions; writing on/off/
 * reset controls it. The complement to strace (which logs WHICH syscalls run):
 * this shows WHERE the CPU goes. Gated by a flag, so zero cost when off.
 */
#pragma once
#include <stdint.h>

void prof_tick(uint64_t rip, uint64_t cs);   /* timer IRQ: sample if on + kernel-mode */
void prof_control(const char *cmd);           /* "on" | "off" | "reset" */
int  prof_format(char *buf, int max);         /* the histogram as text; bytes written */
