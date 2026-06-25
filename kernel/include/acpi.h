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
uint64_t acpi_hpet_base(void);  /* HPET register-block MMIO base from the HPET table, or 0 (M1273) */
/* AML namespace parser (M1284): decode the DSDT's AML into a list of named objects. */
enum { AML_SCOPE = 1, AML_DEVICE, AML_METHOD, AML_NAME, AML_REGION,
       AML_FIELD, AML_PROC, AML_POWERRES, AML_THERMAL, AML_MUTEX, AML_EVENT };
void aml_parse(const uint8_t *dsdt, uint32_t len);  /* parse the DSDT namespace (dsdt = its SDT header) */
int  aml_count(int type);                           /* # objects of `type` (0 = all) */
int  aml_has(const char *seg4);                     /* is a 4-char NameSeg present? */
void acpi_poweroff(void);  /* enter S5: power the machine off. Does not return on success. */
void acpi_reboot(void);    /* ACPI reset register, else 8042 pulse. Does not return on success. */
