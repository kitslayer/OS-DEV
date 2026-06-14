/*
 * io.h — x86 port I/O.
 *
 * Devices like the VGA cursor, the serial port, the PIC, and the keyboard
 * aren't memory-mapped; you talk to them through a separate 64K "I/O port"
 * address space using the `in`/`out` instructions. These inline wrappers are
 * how the rest of the kernel pokes that hardware.
 */
#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* A short, harmless write to an unused port — used to give slow legacy
 * hardware (e.g. the PIC) a moment to settle after a command. */
static inline void io_wait(void) {
    outb(0x80, 0);
}
