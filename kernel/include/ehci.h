/*
 * ehci.h — EHCI (USB 2.0) host-controller driver.
 *
 * Where kernel/usb.c drives a UHCI (USB 1.1) controller through I/O ports and a
 * frame-list / QH / TD model, an EHCI controller (PCI class 0x0C, subclass 0x03,
 * prog-IF 0x20) is a high-speed USB 2.0 host whose registers are MEMORY-MAPPED
 * (MMIO via BAR0) and whose transfers ride an *asynchronous schedule*: a circular
 * list of Queue Heads (QH), each carrying a chain of queue Transfer Descriptors
 * (qTD). This driver brings the controller up, resets + routes a root port, builds
 * the async schedule, and ENUMERATES a high-speed device behind it via control
 * transfers (read its descriptors, assign an address) — entirely separate from,
 * and additive to, the UHCI driver + everything on it.
 *
 * SAFE SCOPE: ehci_init() probes PCI for an EHCI controller; with none (or if
 * bring-up fails) it is a clean no-op (returns -1) that leaves UHCI + its tablet /
 * mass-storage / keyboard untouched. A machine without an EHCI controller boots
 * exactly as before.
 */
#pragma once
#include <stdint.h>

/* Probe the PCI bus for an EHCI controller and bring it up: reset the HC, build
 * the async schedule, route the root ports to EHCI, reset the first populated
 * high-speed port, and enumerate the device behind it over control transfers
 * (GET_DESCRIPTOR / SET_ADDRESS / GET config / SET_CONFIGURATION). Returns 0 if a
 * device was enumerated over EHCI, -1 if no EHCI controller is present or bring-up
 * failed (a clean no-op — UHCI and its devices are unaffected). */
int ehci_init(void);

/* 1 once ehci_init() has successfully brought the controller up (HC running). */
int ehci_is_up(void);

/* Boot-time self-test (mirrors ahci_selftest / nvme_selftest): logs that the HC
 * came up (N_PORTS + HCIVERSION), that a root-port reset succeeded, and the
 * enumerated device descriptor read over EHCI (idVendor / idProduct /
 * bDeviceClass). A clean no-op (logs "none found") if no EHCI controller is
 * attached. */
void ehci_selftest(void);
