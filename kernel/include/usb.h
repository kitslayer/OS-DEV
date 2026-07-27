/* usb.h — UHCI USB controller: an absolute-pointing tablet + the generic
 * control/bulk transfer plumbing kernel/usb_storage.c reuses for a flash disk. */
#pragma once
#include <stdint.h>

/* The largest single BULK data phase, in bytes. usb_storage chunks bigger reads
 * into transfers no larger than this; usb.c sizes its bulk TD pool + bounce
 * buffer to match. 4096 = 8 sectors = one (or, here, exactly one) page. */
#define USB_BULK_MAX 4096

/* Find the UHCI controller, enumerate the usb-tablet, and start polling.
 * Returns 0 on success, -1 if no controller/tablet was usable. */
int  usb_tablet_init(void);

/* Check for a new HID report and update the pointer position (call often). */
void usb_tablet_poll(void);

/* ---- shared UHCI layer (used by usb.c's tablet path AND usb_storage.c) ------
 * These let a second device (a USB flash disk) share the one UHCI controller
 * the tablet uses, without either disturbing the other. */

/* Bring the UHCI controller up once (probe PCI, reset HC, allocate the DMA
 * structures, start it). Idempotent — safe to call from both usb_tablet_init()
 * and usb_storage_init(). Returns 0 if the controller is up, -1 if absent. */
int usb_uhci_init(void);

/* 1 once usb_uhci_init() has successfully brought the controller up. */
int usb_uhci_is_up(void);

/* Reset + enable a single root port (0..usb_uhci_port_count()-1). Returns 1 if a
 * device is connected + enabled there, else 0. USB requires enumerating one port
 * at a time (unaddressed devices all answer address 0), so a caller enables a
 * port, fully enumerates + addresses the device behind it, then moves on. */
int usb_uhci_enable_port(int port);

/* Number of UHCI root ports (2). */
int usb_uhci_port_count(void);

/* The root-port index the tablet claimed, or -1 if none — so usb_storage can
 * skip it and leave the live tablet endpoint undisturbed. */
int usb_uhci_tablet_port(void);

/* --- claimed root ports (M1889) --------------------------------------------
 * usb_uhci_enable_port() resets the port, knocking the device behind it back to
 * address 0 — so re-probing a port whose device another driver already
 * enumerated BREAKS that device. Every driver that successfully enumerates a
 * device must claim its port, and every probe loop must skip claimed ports.
 * (The tablet's port is reported as claimed implicitly.) */
void usb_uhci_claim_port(int port);
int  usb_uhci_port_claimed(int port);

/* Allocate the next free USB device address (1..127), or 0 if exhausted. Shared
 * so the tablet and a flash disk on the same controller never collide. */
uint8_t usb_alloc_address(void);

/* A generic CONTROL transfer to device `addr`, endpoint 0 (max packet
 * `ep0_maxp`). setup[8] is the 8-byte request; data/len is the data stage (may
 * be 0); `in` = device->host. Returns 0 on success, -1 on error/timeout. */
int usb_control_xfer(uint8_t addr, uint16_t ep0_maxp, const uint8_t setup[8],
                     void *data, int len, int in);

/* A generic BULK transfer to device `addr`, endpoint `ep` (max packet `maxp`),
 * moving `len` bytes to/from `buf` (`in` selects IN vs OUT). The data toggle is
 * threaded through *toggle (read + updated). *actual receives the byte count
 * actually transferred (a short IN is thus visible). `len` must be 0..USB_BULK_MAX.
 * Returns 0 on success, -1 on error/stall/timeout. */
int usb_bulk_xfer(uint8_t addr, uint8_t ep, uint16_t maxp, int *toggle,
                  void *buf, int len, int in, int *actual);

/* A single INTERRUPT-IN transfer from device `addr`, endpoint `ep` (max packet
 * `maxp`): read up to `len` bytes (1..64) into `buf`. The data toggle is threaded
 * through *toggle. *actual receives the bytes received — 0 if the endpoint had
 * nothing pending (a NAK), so a poll on an idle keyboard returns 0/empty cleanly
 * WITHOUT spinning and without desyncing the toggle. Runs on a dedicated QH, so
 * it never disturbs the tablet's live interrupt endpoint. Returns 0 on success
 * (including the empty-poll case), -1 on stall/error/bad-arg. */
int usb_interrupt_xfer(uint8_t addr, uint8_t ep, uint16_t maxp, int *toggle,
                       void *buf, int len, int *actual);
