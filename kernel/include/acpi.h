/* acpi.h — minimal ACPI: find the tables at boot, then clean poweroff/reboot.
 *
 * Parses RSDP -> RSDT/XSDT -> FADT, pulls the PM1 control ports + the \_S5
 * sleep-type values out of the DSDT's AML, and the reset register. Enough for a
 * real ACPI S5 power-off and an ACPI reset (with an 8042 fallback) — no AML
 * interpreter, just the well-known \_S5 byte scan. */
#pragma once
#include <stdint.h>

void acpi_init(void);      /* scan the ACPI tables at boot (logs what it found) */
int  acpi_madt_lapics(uint8_t *ids, int max);  /* APIC IDs of all CPUs from the MADT, for SMP (M1197) */
int  acpi_madt_ioapic(uint32_t *addr, uint32_t *gsi_base);  /* first I/O APIC's MMIO base + GSI base; 1/0 (M1856) */
uint32_t acpi_madt_gsi_for_irq(uint8_t irq);   /* ISA IRQ -> GSI (MADT override), identity if none (M1856) */

/* Full MADT Interrupt Source Override lookup for an ISA IRQ (M1890): fills *gsi
 * and the *flags word (bits 1:0 polarity — 00 bus default / 01 high / 11 low;
 * bits 3:2 trigger — 00 bus default / 01 edge / 11 level) and returns 1, or
 * returns 0 if no override names `irq`. Either out-pointer may be NULL. The
 * flags matter: an I/O APIC redirection entry programmed without them is always
 * edge/active-high, which is wrong for any line the firmware remapped. */
int acpi_madt_irq_override(uint8_t irq, uint32_t *gsi, uint16_t *flags);

/* MADT ISO flag fields. */
#define ACPI_MADT_POLARITY(f)  ((f) & 0x3)         /* 0 = bus default, 1 = high, 3 = low  */
#define ACPI_MADT_TRIGGER(f)   (((f) >> 2) & 0x3)  /* 0 = bus default, 1 = edge, 3 = level */
uint64_t acpi_hpet_base(void);  /* HPET register-block MMIO base from the HPET table, or 0 (M1273) */
/* AML namespace parser (M1284): decode the DSDT's AML into a list of named objects. */
enum { AML_SCOPE = 1, AML_DEVICE, AML_METHOD, AML_NAME, AML_REGION,
       AML_FIELD, AML_PROC, AML_POWERRES, AML_THERMAL, AML_MUTEX, AML_EVENT };
void aml_parse(const uint8_t *dsdt, uint32_t len);  /* parse the DSDT namespace (dsdt = its SDT header) */
int  aml_count(int type);                           /* # objects of `type` (0 = all) */
int  aml_has(const char *seg4);                     /* is a 4-char NameSeg present? */
int  aml_obj(int i, char *name_out);                /* i-th object: fills name_out[5], returns AML_* type or -1 (M1285) */
long aml_eval_s5(void);                             /* evaluate the \_S5_ package -> SLP_TYPa|SLP_TYPb<<8, or -1 (M1286) */
/* AML method EVALUATION (M1289): a bytecode VM that actually RUNS control methods. */
void aml_eval_register(const char *name4, const uint8_t *body, uint32_t len, uint8_t argc);  /* register a method by raw body bytes (self-test) */
long aml_eval_call(const char *name4, const uint64_t *args, int nargs);  /* run a method by name -> integer result, or -1 */
void aml_eval_selftest(void);                       /* run + log the FACT/SUMN VM self-test (boot marker) */
int  acpi_s5_values(void);                          /* the same value from acpi.c's byte-scan (cross-check, M1286) */
void acpi_poweroff(void);  /* enter S5: power the machine off. Does not return on success. */
void acpi_reboot(void);    /* ACPI reset register, else 8042 pulse. Does not return on success. */
