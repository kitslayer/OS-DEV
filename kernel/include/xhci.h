/*
 * xhci.h — xHCI (USB 3.0) host-controller driver.
 *
 * This completes the USB host-controller trilogy: kernel/usb.c drives a UHCI
 * (USB 1.1) controller through I/O ports + a frame-list / QH / TD model, and
 * kernel/ehci.c drives an EHCI (USB 2.0) controller via a memory-mapped async
 * QH/qTD schedule. An xHCI controller (PCI class 0x0C, subclass 0x03, prog-IF
 * 0x30) is the modern USB *3.0* host: its registers are MEMORY-MAPPED (MMIO via
 * BAR0) and its transfers ride a fundamentally different structure — rings of
 * Transfer Request Blocks (TRBs): a *command ring* the host walks for controller
 * commands, an *event ring* the controller posts completions onto, and per-device
 * *transfer rings* (one per endpoint) for data. Devices are described by
 * controller-owned *device-context* structures and configured through *input-
 * context* structures. This driver brings the controller up, sets up those rings,
 * resets + detects a root port, and ENUMERATES the device behind it (ENABLE SLOT
 * -> ADDRESS DEVICE -> read its descriptors over xHCI) — entirely separate from,
 * and additive to, the UHCI + EHCI drivers and everything on them.
 *
 * SAFE SCOPE: xhci_init() probes PCI for an xHCI controller; with none (or if
 * bring-up fails) it is a clean no-op (returns -1) that leaves UHCI + EHCI + all
 * their devices untouched. A machine without an xHCI controller boots exactly as
 * before.
 */
#pragma once
#include <stdint.h>

/* Probe the PCI bus for an xHCI controller and bring it up: reset the HC, set up
 * the device-context base-address array + command ring + event ring, run the
 * controller, reset the first populated root port, ENABLE SLOT + ADDRESS DEVICE
 * for the device behind it, and enumerate it over EP0 control transfers
 * (GET_DESCRIPTOR device/config, SET_CONFIGURATION). Returns 0 if a device was
 * enumerated over xHCI, -1 if no xHCI controller is present or bring-up failed (a
 * clean no-op — UHCI/EHCI and their devices are unaffected). */
int xhci_init(void);

/* 1 once xhci_init() has successfully brought the controller up (HC running). */
int xhci_is_up(void);

/* Boot-time self-test (mirrors ehci_selftest): logs that the HC came up
 * (MaxSlots / MaxPorts / HCIVERSION), that the command + event rings work (ENABLE
 * SLOT returned a slot id), a root-port reset, and the enumerated device
 * descriptor read over xHCI (idVendor / idProduct / bDeviceClass). STRETCH: a
 * bulk IN (BOT/SCSI READ(10)) to a usb-storage device behind xHCI. A clean no-op
 * (logs "none found") if no xHCI controller is attached. */
void xhci_selftest(void);
