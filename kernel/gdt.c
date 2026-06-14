/*
 * gdt.c — build and load a proper GDT + TSS.
 *
 * In 64-bit long mode the CPU mostly ignores segmentation: base and limit are
 * forced to 0 / full, so segments no longer carve up memory. But the GDT is
 * still mandatory because a few things are encoded in segment *descriptors*:
 *
 *   - the privilege level (ring 0 kernel vs ring 3 user) of code/data,
 *   - whether a code segment runs in 64-bit mode (the "L" bit),
 *   - and the Task State Segment (TSS).
 *
 * The TSS in long mode no longer holds a task's registers (no hardware task
 * switching here). It holds two things we care about:
 *   - RSP0: the stack the CPU switches to when an interrupt takes us from
 *     ring 3 into ring 0 (needed once we have userspace), and
 *   - the IST table: up to 7 known-good stacks an interrupt gate can demand,
 *     so e.g. a double fault runs on a fresh stack even if RSP was garbage.
 */
#include "gdt.h"
#include "string.h"
#include <stdint.h>

/* 64-bit TSS, packed exactly as the CPU expects (104 bytes). */
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];     /* rsp0..rsp2: stacks for ring 0..2 */
    uint64_t reserved1;
    uint64_t ist[7];     /* ist[0] == IST1 .. ist[6] == IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* The GDT: null, kcode, kdata, ucode, udata, then a 16-byte TSS descriptor
 * that occupies the last two 8-byte slots. */
static uint64_t gdt[7];
static struct tss tss;

/* Stacks the TSS points at. .bss isn't guaranteed zeroed by the loader, but
 * stacks don't care — we only ever write before we read. */
static uint8_t kernel_stack[16384] __attribute__((aligned(16)));
static uint8_t df_stack[8192]      __attribute__((aligned(16)));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Defined in gdt_flush.asm: loads the GDTR and reloads CS/SS/data selectors. */
extern void gdt_flush(struct gdtr *ptr);

/* Encode one ordinary 8-byte descriptor. In long mode base/limit are ignored
 * for code/data, but we fill conventional values anyway. `flags` is the high
 * nibble (G, D/B, L, AVL); `access` is the type/DPL/present byte. */
static uint64_t make_desc(uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    uint64_t d = 0;
    d |= (uint64_t)(limit & 0xFFFF);
    d |= (uint64_t)(base & 0xFFFFFF) << 16;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0xF) << 48;
    d |= (uint64_t)(flags & 0xF) << 52;
    d |= (uint64_t)((base >> 24) & 0xFF) << 56;
    return d;
}

void gdt_init(void) {
    /* access bytes: P=1. code=0x1A|.., data=0x12|.. ; ring in bits 5-6.
     *   0x9A kernel code, 0x92 kernel data, 0xFA user code, 0xF2 user data.
     * flags: code uses L=1 (0xA), data uses D/B=1 (0xC). G=1 in both. */
    gdt[0] = 0;                                   /* null */
    gdt[1] = make_desc(0, 0xFFFFF, 0x9A, 0xA);    /* kernel code */
    gdt[2] = make_desc(0, 0xFFFFF, 0x92, 0xC);    /* kernel data */
    gdt[3] = make_desc(0, 0xFFFFF, 0xFA, 0xA);    /* user code   */
    gdt[4] = make_desc(0, 0xFFFFF, 0xF2, 0xC);    /* user data   */

    /* The TSS itself. */
    memset(&tss, 0, sizeof(tss));
    tss.rsp[0]   = (uint64_t)(kernel_stack + sizeof(kernel_stack));
    tss.ist[0]   = (uint64_t)(df_stack + sizeof(df_stack)); /* IST1 */
    tss.iomap_base = sizeof(tss);                 /* no I/O permission bitmap */

    /* The TSS descriptor is a 16-byte *system* descriptor spanning gdt[5..6].
     * access 0x89 = present, type 0x9 (available 64-bit TSS), G=0. */
    uint64_t base  = (uint64_t)&tss;
    uint32_t limit = sizeof(tss) - 1;
    gdt[5] = make_desc((uint32_t)base, limit, 0x89, 0x0);
    gdt[6] = (base >> 32) & 0xFFFFFFFF;           /* high 32 bits of base */

    struct gdtr gdtr = { .limit = sizeof(gdt) - 1, .base = (uint64_t)&gdt };
    gdt_flush(&gdtr);                             /* lgdt + reload CS/DS/SS */

    __asm__ volatile("ltr %0" : : "r"((uint16_t)TSS_SEL)); /* load task register */
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp[0] = rsp0;
}
