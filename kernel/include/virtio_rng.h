/*
 * virtio_rng.h — virtio-rng paravirtual hardware entropy source.
 *
 * The simplest virtio device: the guest hands the host a device-writable buffer
 * over a single virtqueue and the host fills it with random bytes from its own
 * entropy pool. It's how a VM gets real entropy from the hypervisor rather than
 * relying solely on its own PRNG. We speak the LEGACY (virtio 0.9.5) transport
 * over PCI — a plain I/O-port BAR for the config registers and one split
 * virtqueue handed to the device by page-frame number. Discovered on the PCI
 * bus as vendor 0x1AF4, device 0x1005.
 *
 * Purely additive: virtio_rng_init() probes PCI; with no device attached it is a
 * clean no-op, so a machine without one boots unchanged.
 */
#pragma once
#include <stdint.h>

/* Probe the PCI bus for a legacy virtio entropy device and bring its virtqueue
 * up. Returns 0 if one was initialized, -1 if none is present (a clean no-op). */
int virtio_rng_init(void);

/* 1 if virtio_rng_init() brought a device up, else 0. */
int virtio_rng_present(void);

/* Fill `buf` with up to `max` random bytes from the device. Returns the number
 * of bytes actually written (>0), or -1 on no-device/error/timeout. `buf` must
 * be in identity-mapped low RAM (the device DMAs into its physical frame). */
long virtio_rng_read(void *buf, unsigned long max);

/* Boot-time self-test: if a virtio-rng device is present, draw two batches of
 * entropy and log them, asserting the bytes are nonzero and the batches differ.
 * No-op (logs "none attached") if no device. */
void virtio_rng_selftest(void);
