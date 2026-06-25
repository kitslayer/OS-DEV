/*
 * virtio_console.h — virtio-console paravirtual serial port to the host.
 *
 * The guest writes bytes to the port's transmit virtqueue and they emerge from
 * whatever the hypervisor wired the chardev to (a host file/pipe/pty) — the
 * standard out-of-band host channel under QEMU. We speak the LEGACY (virtio
 * 0.9.5) transport over PCI (I/O-port BAR0 + split virtqueues), negotiating no
 * features so it's a single implicit port (rx=queue 0, tx=queue 1, no control
 * queue). Discovered on the PCI bus as vendor 0x1AF4, device 0x1003.
 *
 * Purely additive: virtio_console_init() probes PCI; with no device attached it
 * is a clean no-op, so a machine without one boots unchanged.
 */
#pragma once
#include <stdint.h>

/* Probe the PCI bus for a legacy virtio console and bring its virtqueues up.
 * Returns 0 if one was initialized, -1 if none is present (a clean no-op). */
int  virtio_console_init(void);

/* 1 if virtio_console_init() brought a device up, else 0. */
int  virtio_console_present(void);

/* Emit `len` bytes out the console transmit queue (the host chardev receives
 * them). Returns bytes written (>=0) or -1. */
long virtio_console_write(const void *buf, unsigned long len);

/* Boot-time self-test: if a console is present, write a recognizable line out
 * the transmit queue and log a marker. No-op if no device. */
void virtio_console_selftest(void);
