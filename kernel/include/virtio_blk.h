/*
 * virtio_blk.h — virtio-blk paravirtual block device (the standard fast VM disk).
 *
 * Where kernel/ata.c drives a real IDE controller in PIO and kernel/ahci.c drives
 * a SATA disk over an MMIO DMA HBA, virtio-blk is the disk a hypervisor *offers*
 * directly: instead of emulating register-level hardware, the guest and host
 * share a "virtqueue" of descriptors in RAM and exchange block requests through
 * it. It's the fastest, simplest disk under QEMU/KVM. We speak the LEGACY
 * (virtio 0.9.5) transport over PCI: a plain I/O-port BAR for the config
 * registers and a single split virtqueue whose page is handed to the device by
 * page-frame number. Discovered on the PCI bus as vendor 0x1AF4, device 0x1001.
 *
 * This is an *additional* block device; FAT32/VFS stay on the legacy ATA boot
 * disk (kernel/ata.c). virtio_blk_init() probes PCI for a virtio block device;
 * if there's none it is a clean no-op, so a machine without one boots unchanged.
 */
#pragma once
#include <stdint.h>

#define VIRTIO_BLK_SECTOR_SIZE 512

/* Probe the PCI bus for a legacy virtio block device and bring its virtqueue up.
 * Returns 0 if a usable device was initialized, -1 if none is present (a clean
 * no-op, so the legacy-ATA-only boot still works). */
int virtio_blk_init(void);

/* 1 if virtio_blk_init() brought a device up, else 0. */
int virtio_blk_present(void);

/* Capacity of the device, in 512-byte sectors (0 if no device). */
uint64_t virtio_blk_capacity(void);

/* Read `count` 512-byte sectors starting at LBA `lba` into `buf` (must hold
 * count*512 bytes, in identity-mapped low RAM — the device DMAs into its
 * physical frame). Returns 0 on success, -1 on error/timeout/bad-arg. */
int virtio_blk_read(uint64_t lba, uint32_t count, void *buf);

/* Write `count` sectors from `buf` to `lba`. Same buffer constraints as read.
 * Returns 0 on success, -1 otherwise. */
int virtio_blk_write(uint64_t lba, uint32_t count, const void *buf);

/* Boot-time self-test: if a virtio block device is present, read sector 0 (and a
 * couple more) and log the first bytes + a checksum, so the read can be matched
 * against known on-disk content. No-op (logs "none attached") if no device. */
void virtio_blk_selftest(void);
