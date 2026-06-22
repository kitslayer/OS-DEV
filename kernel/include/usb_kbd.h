/*
 * usb_kbd.h — a USB HID boot-protocol keyboard over the UHCI host controller
 * (kernel/usb.c), feeding the SAME kernel input queue (keyboard.c's input_push)
 * the PS/2 keyboard and serial line use — so a USB keyboard drives the shell and
 * apps identically to the PS/2 one.
 *
 * Where kernel/keyboard.c drives a PS/2 keyboard off IRQ1, this enumerates a HID
 * boot keyboard (interface class 0x03, subclass 0x01 = Boot, protocol 0x01 =
 * Keyboard) on the USB bus, finds its INTERRUPT IN endpoint, puts it in boot
 * protocol (HID SET_PROTOCOL 0), and polls that endpoint for the 8-byte boot
 * report. New key-DOWN events (diffed against the previous report) are translated
 * via a static HID-usage->ASCII table (with a shifted variant) and pushed into
 * the shared input queue. QEMU's `-device usb-kbd` presents exactly this.
 *
 * This is purely ADDITIVE: usb_kbd_init() probes the USB bus for a HID boot
 * keyboard, SKIPPING the port the tablet claimed and using the shared USB address
 * allocator; if there's no keyboard it is a clean no-op, so a machine without a
 * USB keyboard boots unchanged and the PS/2 keyboard + USB tablet + USB
 * mass-storage paths are untouched.
 */
#pragma once

/* Bring the UHCI controller up (shared with the tablet/storage; idempotent),
 * enumerate a USB HID boot keyboard on the bus, put it in boot protocol, and arm
 * its interrupt endpoint. Returns 0 if a usable boot keyboard was found, -1 if
 * none is present (a clean no-op). */
int usb_kbd_init(void);

/* 1 if usb_kbd_init() brought a keyboard up, else 0. */
int usb_kbd_present(void);

/* Poll the interrupt-IN endpoint once: if a new report is pending, decode the
 * newly-pressed keys and push them into the shared input queue. Non-blocking — a
 * poll with no new report returns immediately. Call often (alongside
 * usb_tablet_poll). No-op if no keyboard. */
void usb_kbd_poll(void);

/* Boot-time self-test / log line: report whether a USB HID boot keyboard was
 * enumerated (class/subclass/proto + its interrupt endpoint), or that none was
 * attached (a clean no-op — PS/2 keyboard + tablet intact). */
void usb_kbd_selftest(void);
