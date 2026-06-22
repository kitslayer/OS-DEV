/*
 * nvme.h — NVMe block-storage driver (modern PCIe storage, DMA via MMIO doorbells).
 *
 * Where kernel/ata.c drives the legacy IDE controller in PIO and kernel/ahci.c
 * drives a SATA disk over an MMIO AHCI HBA, NVMe is the native PCIe storage
 * interface: a controller whose registers are memory-mapped (a 64-bit BAR0) and
 * which is driven through *queue pairs* (a submission queue + a completion queue)
 * in RAM. We ring a "doorbell" register to tell the controller a command is
 * waiting in a submission queue, it DMAs the data and posts a completion entry in
 * the completion queue, and we poll that queue (the phase bit toggles each wrap).
 * Discovered on the PCI bus as class 0x01, subclass 0x08, prog-IF 0x02 (QEMU's
 * `-device nvme` is vendor 0x1B36, device 0x0010).
 *
 * This is an *additional* block device; FAT32/VFS stay on the legacy ATA boot
 * disk (kernel/ata.c). nvme_init() probes PCI for an NVMe controller; if there's
 * none it is a clean no-op, so a machine without one boots unchanged. Mirrors
 * kernel/ahci.c's and kernel/virtio_blk.c's additive shape.
 */
#pragma once
#include <stdint.h>

#define NVME_SECTOR_SIZE 512   /* the LBA size we assume for the public API/tests */

/* Probe the PCI bus for an NVMe controller, bring it up (admin + one IO queue
 * pair), and identify namespace 1. Returns 0 if a usable namespace was brought
 * up, or -1 if no NVMe controller is present / bring-up failed (a clean no-op,
 * so the legacy-ATA-only boot still works). */
int nvme_init(void);

/* 1 if nvme_init() brought a namespace up, else 0. */
int nvme_present(void);

/* Capacity of namespace 1, in NVME_SECTOR_SIZE-byte sectors (0 if no device). */
uint64_t nvme_capacity(void);

/* Read `count` 512-byte sectors starting at LBA `lba` into `buf` (must hold
 * count*512 bytes). Internally DMAs through a page-aligned bounce frame and
 * memcpy()s into `buf`, so `buf` has no alignment constraint. Returns 0 on
 * success, -1 on error/timeout/bad-arg. */
int nvme_read(uint64_t lba, uint32_t count, void *buf);

/* Write `count` sectors from `buf` to `lba`. Same buffer rules as nvme_read.
 * Returns 0 on success, -1 otherwise. */
int nvme_write(uint64_t lba, uint32_t count, const void *buf);

/* Boot-time self-test: if an NVMe namespace is present, log its capacity + LBA
 * size, read sectors 0..2 and log the first bytes + a checksum (so the read can
 * be matched against known on-disk content), and do a write round-trip on the
 * last sector (write a marker, read it back, restore). No-op (logs "none found")
 * if no NVMe controller is attached. Mirrors ahci_selftest / virtio_blk_selftest. */
void nvme_selftest(void);
