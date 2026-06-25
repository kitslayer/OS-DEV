/*
 * multiboot.h — the (subset of the) Multiboot 1 information structure that the
 * bootloader hands us in a register at boot. We only declare the fields we
 * actually read; the real struct continues past mmap_addr.
 */
#pragma once
#include <stdint.h>

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;     /* KiB of low memory (valid if flags bit 0)  */
    uint32_t mem_upper;     /* KiB of memory above 1 MiB                 */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;   /* bytes (valid if flags bit 6)              */
    uint32_t mmap_addr;     /* physical address of the first mmap entry  */
    /* offsets 52..86 — drives / config / boot-loader-name / APM / VBE: declared
     * (unused) only so the framebuffer fields below land at their spec offset 88. */
    uint32_t drives_length, drives_addr, config_table, boot_loader_name, apm_table;
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    /* framebuffer (valid if flags bit 12) — what GRUB / QEMU fill in when the
     * Multiboot header requests a video mode. The key to bare-metal graphics. */
    uint64_t framebuffer_addr;     /* offset 88 */
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;     /* 1 = direct RGB, 0 = indexed, 2 = EGA text */
} __attribute__((packed));

/* One entry in the memory map. Note `size` excludes itself, so the next entry
 * is at (entry_addr + size + 4) — entries are NOT a fixed stride. */
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;          /* 1 == available RAM */
} __attribute__((packed));

#define MULTIBOOT_FLAG_MEM   (1u << 0)
#define MULTIBOOT_FLAG_MMAP  (1u << 6)
#define MULTIBOOT_FLAG_FB    (1u << 12)   /* framebuffer_* fields are valid (bare-metal graphics) */
#define MULTIBOOT_MEM_AVAILABLE 1
