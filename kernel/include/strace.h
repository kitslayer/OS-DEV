/*
 * strace.h — a per-process syscall trace RING (M1118).
 *
 * The strace channel (M1084) only ever printed to the kernel log (dmesg), which
 * is volatile and interleaved with everything else. This adds a small in-memory
 * ring of recent traced syscalls, tagged by pid, so a program can read its own
 * (or another pid's) recent calls as a file: /proc/<pid>/strace. Records are
 * appended only while a process is being traced (toggle via /proc/<pid>/ctl).
 */
#pragma once
#include <stdint.h>

/* Append one traced syscall to the ring (called from syscall_dispatch). */
void strace_record(int pid, const char *name, uint64_t a, uint64_t b, uint64_t c, uint64_t ret);

/* Format the ring into `buf`, oldest-first. pid>0 filters to that process;
 * pid<=0 emits every record (with a pid column). Returns bytes written. */
int  strace_format(int pid, char *buf, int max);
