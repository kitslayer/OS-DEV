/*
 * acpi.c — just enough ACPI for a clean power-off and an ACPI reset.
 *
 * Hobby-OS milestone (r/osdev): once an OS boots, drives hardware and networks,
 * "it can actually turn itself off" is the next satisfying systems feature. We
 * had only an 8042 reset pulse and no power-off at all. This walks the firmware
 * tables — RSDP -> RSDT/XSDT -> FADT -> DSDT — and:
 *   - reads the PM1a/PM1b control ports + the \_S5 SLP_TYP values (the classic
 *     non-interpreting AML byte scan) so we can enter the S5 "soft off" state;
 *   - reads the FADT reset register (+ value) for an ACPI reboot.
 * No AML interpreter; the \_S5 scan is the widely-used heuristic. All firmware
 * memory is reached through the higher-half direct map (hhdm), and every table
 * read is bounded by the table's own length field.
 */
#include "acpi.h"
#include "io.h"
#include "vmm.h"
#include "console.h"
#include <stdint.h>

struct rsdp {
    char     sig[8];          /* "RSD PTR " */
    uint8_t  checksum;        /* sum of the first 20 bytes == 0 */
    char     oemid[6];
    uint8_t  revision;        /* 0 = ACPI 1.0 (use rsdt), >=2 = use xsdt */
    uint32_t rsdt_addr;
    uint32_t length;          /* 2.0+ */
    uint64_t xsdt_addr;       /* 2.0+ */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oemtableid[8];
    uint32_t oemrevision;
    uint32_t creatorid;
    uint32_t creatorrevision;
} __attribute__((packed));

struct gas {                  /* ACPI Generic Address Structure */
    uint8_t  space_id;        /* 0 = system memory, 1 = system I/O */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

struct fadt {
    struct sdt_header h;          /*   0 */
    uint32_t firmware_ctrl;       /*  36 */
    uint32_t dsdt;                /*  40 */
    uint8_t  reserved0;           /*  44 */
    uint8_t  preferred_pm_profile;/*  45 */
    uint16_t sci_int;             /*  46 */
    uint32_t smi_cmd;             /*  48 */
    uint8_t  acpi_enable;         /*  52 */
    uint8_t  acpi_disable;        /*  53 */
    uint8_t  s4bios_req;          /*  54 */
    uint8_t  pstate_cnt;          /*  55 */
    uint32_t pm1a_evt_blk;        /*  56 */
    uint32_t pm1b_evt_blk;        /*  60 */
    uint32_t pm1a_cnt_blk;        /*  64 */
    uint32_t pm1b_cnt_blk;        /*  68 */
    uint32_t pm2_cnt_blk;         /*  72 */
    uint32_t pm_tmr_blk;          /*  76 */
    uint32_t gpe0_blk;            /*  80 */
    uint32_t gpe1_blk;            /*  84 */
    uint8_t  pm1_evt_len;         /*  88 */
    uint8_t  pm1_cnt_len;         /*  89 */
    uint8_t  pm2_cnt_len;         /*  90 */
    uint8_t  pm_tmr_len;          /*  91 */
    uint8_t  gpe0_blk_len;        /*  92 */
    uint8_t  gpe1_blk_len;        /*  93 */
    uint8_t  gpe1_base;           /*  94 */
    uint8_t  cst_cnt;             /*  95 */
    uint16_t p_lvl2_lat;          /*  96 */
    uint16_t p_lvl3_lat;          /*  98 */
    uint16_t flush_size;          /* 100 */
    uint16_t flush_stride;        /* 102 */
    uint8_t  duty_offset;         /* 104 */
    uint8_t  duty_width;          /* 105 */
    uint8_t  day_alrm;            /* 106 */
    uint8_t  mon_alrm;            /* 107 */
    uint8_t  century;             /* 108 */
    uint16_t iapc_boot_arch;      /* 109 */
    uint8_t  reserved1;           /* 111 */
    uint32_t flags;               /* 112 */
    struct gas reset_reg;         /* 116 */
    uint8_t  reset_value;         /* 128 */
    uint8_t  reserved2[3];        /* 129 */
    uint64_t x_firmware_ctrl;     /* 132 */
    uint64_t x_dsdt;              /* 140 */
} __attribute__((packed));

#define FADT_FLAG_RESET_REG_SUP (1u << 10)
#define ACPI_SLP_EN             (1u << 13)

/* parsed once at boot */
static uint16_t pm1a_cnt, pm1b_cnt;
static uint8_t  slp_typa, slp_typb;
static int      s5_ok;
static uint8_t  reset_space;     /* GAS space id */
static uint64_t reset_addr;
static uint8_t  reset_value;
static int      reset_ok;

static int checksum_ok(const uint8_t *p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s += p[i];
    return s == 0;
}

static int sig4(const char *a, const char *b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

/* Scan a physical range for the "RSD PTR " signature on 16-byte boundaries. */
static struct rsdp *scan_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t p = start; p < end; p += 16) {
        struct rsdp *r = (struct rsdp *)hhdm(p);
        if (r->sig[0]=='R'&&r->sig[1]=='S'&&r->sig[2]=='D'&&r->sig[3]==' '&&
            r->sig[4]=='P'&&r->sig[5]=='T'&&r->sig[6]=='R'&&r->sig[7]==' ' &&
            checksum_ok((const uint8_t *)r, 20))
            return r;
    }
    return 0;
}

static struct rsdp *find_rsdp(void) {
    /* 1) the first KiB of the EBDA (segment pointer at physical 0x40E) */
    uint16_t ebda_seg = *(volatile uint16_t *)hhdm(0x40E);
    if (ebda_seg) {
        uint64_t ebda = (uint64_t)ebda_seg << 4;
        struct rsdp *r = scan_rsdp(ebda, ebda + 1024);
        if (r) return r;
    }
    /* 2) the BIOS read-only area 0xE0000 - 0xFFFFF */
    return scan_rsdp(0xE0000, 0x100000);
}

/* Pull SLP_TYPa/SLP_TYPb out of the DSDT's \_S5 package (no AML interpreter,
 * just the classic byte scan, fully bounded by the DSDT length). */
static void parse_s5(uint32_t dsdt_phys) {
    if (!dsdt_phys) return;
    const struct sdt_header *h = (const struct sdt_header *)hhdm(dsdt_phys);
    if (!sig4(h->sig, "DSDT")) return;
    uint32_t len = h->length;
    if (len < 36 || len > (16u << 20)) return;          /* sane bound */
    const uint8_t *aml = (const uint8_t *)h;
    aml_parse(aml, len);                                /* M1284: decode the full DSDT namespace (the real AML walk) */
    aml_eval_selftest();                                /* M1289: prove the AML method-evaluation VM (logs a marker) */
    for (uint32_t i = 36; i + 8 < len; i++) {           /* leave room for the package bytes */
        if (aml[i]=='_'&&aml[i+1]=='S'&&aml[i+2]=='5'&&aml[i+3]=='_') {
            /* must be a Name: NameOp(0x08) [\\] "_S5_" PackageOp(0x12) */
            int named = (i >= 1 && aml[i-1]==0x08) ||
                        (i >= 2 && aml[i-2]==0x08 && aml[i-1]=='\\');
            if (!named || aml[i+4] != 0x12) continue;
            uint32_t p = i + 5;                          /* at PkgLength */
            p += ((aml[p] & 0xC0) >> 6) + 2;             /* skip PkgLength + NumElements */
            if (p >= len) return;
            if (aml[p] == 0x0A) p++;                     /* BytePrefix before SLP_TYPa */
            if (p >= len) return;
            slp_typa = aml[p++];
            if (p < len && aml[p] == 0x0A) p++;          /* BytePrefix before SLP_TYPb */
            if (p < len) slp_typb = aml[p];
            s5_ok = 1;
            return;
        }
    }
}

/* The \_S5_ SLP_TYP values found by the byte-scan above, packed SLP_TYPa |
 * SLP_TYPb<<8, or -1 if no _S5_. The cross-check target for aml_eval_s5 (M1286). */
int acpi_s5_values(void) { return s5_ok ? (slp_typa | (slp_typb << 8)) : -1; }

static void parse_fadt(const struct fadt *f) {
    pm1a_cnt = (uint16_t)f->pm1a_cnt_blk;
    pm1b_cnt = (uint16_t)f->pm1b_cnt_blk;
    /* reset register (ACPI 2.0+: FADT long enough + flag set) */
    if (f->h.length > 128 && (f->flags & FADT_FLAG_RESET_REG_SUP)) {
        reset_space = f->reset_reg.space_id;
        reset_addr  = f->reset_reg.address;
        reset_value = f->reset_value;
        if (reset_addr) reset_ok = 1;
    }
    /* prefer the 64-bit DSDT pointer when present + sane */
    uint32_t dsdt = f->dsdt;
    if (f->h.length > 147 && f->x_dsdt && f->x_dsdt < (1ull << 32)) dsdt = (uint32_t)f->x_dsdt;   /* x_dsdt is 8 bytes at offset 140 (bytes 140-147) -- was >140, which only guarantees the FIRST of those 8 bytes is within the table */
    parse_s5(dsdt);
}

/* Enumerate the usable processors from the MADT (the ACPI table signed "APIC")
 * for SMP bring-up (kernel/smp.c). Walks the XSDT/RSDT for the MADT, then its
 * variable-length entry list, collecting the APIC ID of every Processor Local
 * APIC (entry type 0) that is Enabled or Online-Capable. Returns the count (the
 * BSP is included — smp.c skips its own ID), or 0 if there's no MADT. M1197. */
int acpi_madt_lapics(uint8_t *ids, int max) {
    struct rsdp *r = find_rsdp();
    if (!r) return 0;

    const struct sdt_header *madt = 0;
    if (r->revision >= 2 && r->xsdt_addr) {              /* XSDT: 64-bit entries */
        const struct sdt_header *x = (const struct sdt_header *)hhdm(r->xsdt_addr);
        if (sig4(x->sig, "XSDT") && x->length >= 36 && x->length < (1u << 20)) {
            uint32_t n = (x->length - 36) / 8;
            const uint8_t *ents = (const uint8_t *)x + 36;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t ep; __builtin_memcpy(&ep, ents + i*8, 8);
                const struct sdt_header *t = (const struct sdt_header *)hhdm(ep);
                if (sig4(t->sig, "APIC")) { madt = t; break; }
            }
        }
    }
    if (!madt && r->rsdt_addr) {                         /* RSDT: 32-bit entries */
        const struct sdt_header *rs = (const struct sdt_header *)hhdm(r->rsdt_addr);
        if (sig4(rs->sig, "RSDT") && rs->length >= 36 && rs->length < (1u << 20)) {
            uint32_t n = (rs->length - 36) / 4;
            const uint8_t *ents = (const uint8_t *)rs + 36;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ep; __builtin_memcpy(&ep, ents + i*4, 4);
                const struct sdt_header *t = (const struct sdt_header *)hhdm(ep);
                if (sig4(t->sig, "APIC")) { madt = t; break; }
            }
        }
    }
    if (!madt || madt->length < 44 || madt->length >= (1u << 20)) return 0;

    /* MADT entries begin after the 36-byte SDT header + 8 bytes (local APIC
     * address + flags). Each entry is [u8 type][u8 length(incl. header)]; type 0
     * = Processor Local APIC: { type, len=8, acpi_id, apic_id, u32 flags }. */
    int cnt = 0;
    const uint8_t *p   = (const uint8_t *)madt + 44;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end) break;             /* malformed -> stop */
        if (type == 0 && len >= 8) {
            uint32_t flags; __builtin_memcpy(&flags, p + 4, 4);
            if ((flags & 1) || (flags & 2)) {            /* Enabled | Online-Capable */
                if (cnt < max) ids[cnt++] = p[3];        /* apic_id */
            }
        }
        p += len;
    }
    return cnt;
}

/* Find the MADT ("APIC") table via the XSDT/RSDT (M1856). NULL if absent/bad.
 * A local copy of acpi_madt_lapics's table-find so the working SMP path above
 * stays untouched. */
static const struct sdt_header *find_madt(void) {
    struct rsdp *r = find_rsdp();
    if (!r) return 0;
    const struct sdt_header *madt = 0;
    if (r->revision >= 2 && r->xsdt_addr) {
        const struct sdt_header *x = (const struct sdt_header *)hhdm(r->xsdt_addr);
        if (sig4(x->sig, "XSDT") && x->length >= 36 && x->length < (1u << 20)) {
            uint32_t n = (x->length - 36) / 8;
            const uint8_t *ents = (const uint8_t *)x + 36;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t ep; __builtin_memcpy(&ep, ents + i*8, 8);
                const struct sdt_header *t = (const struct sdt_header *)hhdm(ep);
                if (sig4(t->sig, "APIC")) { madt = t; break; }
            }
        }
    }
    if (!madt && r->rsdt_addr) {
        const struct sdt_header *rs = (const struct sdt_header *)hhdm(r->rsdt_addr);
        if (sig4(rs->sig, "RSDT") && rs->length >= 36 && rs->length < (1u << 20)) {
            uint32_t n = (rs->length - 36) / 4;
            const uint8_t *ents = (const uint8_t *)rs + 36;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ep; __builtin_memcpy(&ep, ents + i*4, 4);
                const struct sdt_header *t = (const struct sdt_header *)hhdm(ep);
                if (sig4(t->sig, "APIC")) { madt = t; break; }
            }
        }
    }
    if (!madt || madt->length < 44 || madt->length >= (1u << 20)) return 0;
    return madt;
}

/* First I/O APIC from the MADT (type 1: {type,len=12, id, resv, u32 addr, u32
 * gsi_base}). 1 + fills the addr + gsi_base out-params, else 0. (M1856) */
int acpi_madt_ioapic(uint32_t *addr, uint32_t *gsi_base) {
    const struct sdt_header *madt = find_madt();
    if (!madt) return 0;
    const uint8_t *p = (const uint8_t *)madt + 44, *end = (const uint8_t *)madt + madt->length;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end) break;
        if (type == 1 && len >= 12) {
            uint32_t a, g; __builtin_memcpy(&a, p + 4, 4); __builtin_memcpy(&g, p + 8, 4);
            if (addr)     *addr     = a;      /* two independent guards — kept on */
            if (gsi_base) *gsi_base = g;      /* separate lines (-Wmisleading-indentation) */
            return 1;
        }
        p += len;
    }
    return 0;
}

/* Look up an ISA IRQ's MADT Interrupt Source Override (type 2:
 * {type, len=10, bus, source(irq), u32 gsi, u16 flags}). Returns 1 and fills
 * *gsi / *flags if an override names `irq`, else 0 (identity mapping, bus
 * defaults). Either out-pointer may be NULL. (M1890)
 *
 * The `flags` word is the part that used to be dropped on the floor, and it is
 * the part the I/O APIC actually needs: bits 1:0 are the polarity (00 = bus
 * default, 01 = active high, 11 = active low) and bits 3:2 the trigger mode
 * (00 = bus default, 01 = edge, 11 = level). Programming a redirection entry
 * without them means every routed line is edge/active-high whatever the
 * firmware said — fine for a plain ISA line, wrong for anything the firmware
 * remapped to level/low. */
int acpi_madt_irq_override(uint8_t irq, uint32_t *gsi, uint16_t *flags) {
    const struct sdt_header *madt = find_madt();
    if (!madt) return 0;
    const uint8_t *p = (const uint8_t *)madt + 44, *end = (const uint8_t *)madt + madt->length;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end) break;
        if (type == 2 && len >= 10 && p[3] == irq) {
            if (gsi)   { uint32_t g; __builtin_memcpy(&g, p + 4, 4); *gsi = g; }
            if (flags) { uint16_t f; __builtin_memcpy(&f, p + 8, 2); *flags = f; }
            return 1;
        }
        p += len;
    }
    return 0;
}

/* Map an ISA IRQ to its Global System Interrupt; returns `irq` itself (identity)
 * if no override names it. (M1856) */
uint32_t acpi_madt_gsi_for_irq(uint8_t irq) {
    uint32_t gsi;
    return acpi_madt_irq_override(irq, &gsi, 0) ? gsi : irq;
}

/* Locate the HPET's MMIO base address from the ACPI "HPET" table (M1273). The
 * table is the 36-byte SDT header + a u32 event-timer-block-id, then a 12-byte
 * Generic Address Structure whose 64-bit `address` field (table offset 44) is
 * the register block's physical base. Returns 0 if there's no HPET table. */
uint64_t acpi_hpet_base(void) {
    struct rsdp *r = find_rsdp();
    if (!r) return 0;
    const struct sdt_header *hpet = 0;
    if (r->revision >= 2 && r->xsdt_addr) {              /* XSDT: 64-bit entries */
        const struct sdt_header *x = (const struct sdt_header *)hhdm(r->xsdt_addr);
        if (sig4(x->sig, "XSDT") && x->length >= 36 && x->length < (1u << 20)) {
            uint32_t n = (x->length - 36) / 8;
            const uint8_t *ents = (const uint8_t *)x + 36;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t ep; __builtin_memcpy(&ep, ents + i*8, 8);
                const struct sdt_header *t = (const struct sdt_header *)hhdm(ep);
                if (sig4(t->sig, "HPET")) { hpet = t; break; }
            }
        }
    }
    if (!hpet && r->rsdt_addr) {                         /* RSDT: 32-bit entries */
        const struct sdt_header *rs = (const struct sdt_header *)hhdm(r->rsdt_addr);
        if (sig4(rs->sig, "RSDT") && rs->length >= 36 && rs->length < (1u << 20)) {
            uint32_t n = (rs->length - 36) / 4;
            const uint8_t *ents = (const uint8_t *)rs + 36;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ep; __builtin_memcpy(&ep, ents + i*4, 4);
                const struct sdt_header *t = (const struct sdt_header *)hhdm(ep);
                if (sig4(t->sig, "HPET")) { hpet = t; break; }
            }
        }
    }
    if (!hpet || hpet->length < 52) return 0;            /* need at least through the GAS address */
    uint64_t base; __builtin_memcpy(&base, (const uint8_t *)hpet + 44, 8);
    return base;
}

void acpi_init(void) {
    struct rsdp *r = find_rsdp();
    if (!r) { kprintf("[acpi] no RSDP found (poweroff falls back to emulator ports).\n"); return; }

    const struct fadt *fadt = 0;
    if (r->revision >= 2 && r->xsdt_addr) {              /* XSDT: 64-bit entries */
        const struct sdt_header *x = (const struct sdt_header *)hhdm(r->xsdt_addr);
        if (sig4(x->sig, "XSDT") && x->length >= 36 && x->length < (1u << 20)) {
            uint32_t n = (x->length - 36) / 8;
            const uint8_t *ents = (const uint8_t *)x + 36;  /* unaligned u64 entries */
            for (uint32_t i = 0; i < n; i++) {
                uint64_t ep; __builtin_memcpy(&ep, ents + i*8, 8);
                const struct sdt_header *t = (const struct sdt_header *)hhdm(ep);
                if (sig4(t->sig, "FACP")) { fadt = (const struct fadt *)t; break; }
            }
        }
    }
    if (!fadt && r->rsdt_addr) {                         /* RSDT: 32-bit entries */
        const struct sdt_header *rs = (const struct sdt_header *)hhdm(r->rsdt_addr);
        if (sig4(rs->sig, "RSDT") && rs->length >= 36 && rs->length < (1u << 20)) {
            uint32_t n = (rs->length - 36) / 4;
            const uint8_t *ents = (const uint8_t *)rs + 36;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ep; __builtin_memcpy(&ep, ents + i*4, 4);
                const struct sdt_header *t = (const struct sdt_header *)hhdm(ep);
                if (sig4(t->sig, "FACP")) { fadt = (const struct fadt *)t; break; }
            }
        }
    }
    if (!fadt) { kprintf("[acpi] no FADT found.\n"); return; }
    parse_fadt(fadt);

    if (s5_ok)
        kprintf("[ ok ] ACPI: S5 power-off ready (PM1a=0x%x SLP_TYPa=%d)%s.\n",
                pm1a_cnt, slp_typa, reset_ok ? ", reset reg present" : "");
    else
        kprintf("[acpi] FADT found but \\_S5 not parsed; using emulator poweroff fallback.\n");
}

void acpi_poweroff(void) {
    /* parsed path: enter S5 via the real PM1 control register(s) */
    if (s5_ok && pm1a_cnt) {
        outw(pm1a_cnt, (uint16_t)(((uint16_t)slp_typa << 10) | ACPI_SLP_EN));
        if (pm1b_cnt) outw(pm1b_cnt, (uint16_t)(((uint16_t)slp_typb << 10) | ACPI_SLP_EN));
    }
    /* belt-and-suspenders emulator power-off ports (in case parsing failed) */
    outw(0x604, 0x2000);    /* QEMU (PIIX4 PM1a_CNT) */
    outw(0xB004, 0x2000);   /* Bochs */
    outw(0x4004, 0x3400);   /* VirtualBox */
    for (;;) __asm__ volatile ("cli; hlt");
}

void acpi_reboot(void) {
    if (reset_ok) {
        if (reset_space == 1)       outb((uint16_t)reset_addr, reset_value);             /* I/O space */
        else if (reset_space == 0)  *(volatile uint8_t *)hhdm(reset_addr) = reset_value; /* MMIO */
    }
    outb(0xCF9, 0x06);      /* PCI reset control (many chipsets, incl. QEMU q35) */
    outb(0x64, 0xFE);       /* 8042 keyboard-controller reset pulse */
    for (;;) __asm__ volatile ("cli; hlt");
}
