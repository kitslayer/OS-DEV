/* watchdog.h — hardware watchdog + panic-auto-reboot for autonomous real-hardware
 * bring-up (M1881). When enabled (the `watchdog` cmdline flag, set by the PXE
 * bring-up image), a hang or crash self-heals: the machine resets and PXE-boots
 * the latest published kernel, so no human power-cycle is needed. */
#pragma once

void watchdog_enable(unsigned secs);  /* arm the HW watchdog (~secs) + enable panic-auto-reboot */
void watchdog_pet(void);              /* reload the timer — call from the PIT IRQ; no-op if not armed */
int  watchdog_enabled(void);          /* 1 => the panic handler should reboot instead of halting */
