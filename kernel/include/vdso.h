/*
 * vdso.h — a virtual dynamic shared object: one read-only "time page" mapped at
 * a fixed address into EVERY userspace address space. The timer IRQ refreshes it
 * via a seqlock, so a process reads the clock with NO syscall (the way Linux's
 * vDSO serves clock_gettime/gettimeofday). M1111.
 */
#pragma once
#include <stdint.h>

/* The fixed user virtual address of the time page: 2 GiB. It must sit in a PDPT
 * slot the boot identity map leaves empty — i.e. at/above 1 GiB (the low 1 GiB,
 * PDPT[0], is identity-mapped with shared huge pages, so a private 4 KiB leaf
 * can't be installed there) — and clear of the app text (0x40000000), heap/stack
 * (→0x50080000), and mmap (0x60000000+). 0x80000000 (PDPT[2]) satisfies both:
 * boot-empty so it's private per address space, with 512 MiB of mmap headroom
 * below it. ulib hard-codes the same constant so userspace reads with no handshake. */
#define VDSO_ADDR  0x80000000ull

/* The page layout. A reader takes a snapshot between two even `seq` values
 * (odd = an update is in flight); the kernel writer bumps seq odd, writes the
 * fields, then bumps it even. Keep this in sync with the copy in ulib. */
struct vdso_time {
    volatile uint32_t seq;        /* seqlock sequence (odd = update in progress) */
    uint32_t          hz;         /* timer tick frequency (e.g. 100) */
    uint64_t          ticks;      /* monotonic ticks since boot */
    uint64_t          mono_ns;    /* CLOCK_MONOTONIC: nanoseconds since boot */
    uint64_t          real_sec;   /* CLOCK_REALTIME: Unix (UTC) seconds */
    uint32_t          real_nsec;  /* CLOCK_REALTIME: sub-second nanoseconds */
    uint32_t          _pad;
};

void     vdso_init(void);             /* alloc the page + seed the wall-clock base (call once, after vmm+rtc) */
void     vdso_tick(uint64_t ticks);   /* refresh the page from the timer IRQ (cheap, IRQ-safe) */
void     vdso_map(uint64_t cr3);      /* map the page RO|USER|NX into a new address space */
void     vdso_set_realtime(uint64_t unix_sec);  /* re-base the wall clock (e.g. after SNTP sets the RTC) */
