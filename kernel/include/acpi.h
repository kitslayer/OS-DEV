/* acpi.h — minimal ACPI: find the tables at boot, then clean poweroff/reboot.
 *
 * Parses RSDP -> RSDT/XSDT -> FADT, pulls the PM1 control ports + the \_S5
 * sleep-type values out of the DSDT's AML, and the reset register. Enough for a
 * real ACPI S5 power-off and an ACPI reset (with an 8042 fallback) — no AML
 * interpreter, just the well-known \_S5 byte scan. */
#pragma once

void acpi_init(void);      /* scan the ACPI tables at boot (logs what it found) */
void acpi_poweroff(void);  /* enter S5: power the machine off. Does not return on success. */
void acpi_reboot(void);    /* ACPI reset register, else 8042 pulse. Does not return on success. */
