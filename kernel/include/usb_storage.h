/*
 * usb_storage.h — a USB mass-storage class driver: a USB flash disk, read over
 * Bulk-Only Transport (BOT) carrying SCSI commands, on top of the UHCI host
 * controller (kernel/usb.c).
 *
 * Where kernel/ata.c / kernel/ahci.c / kernel/virtio_blk.c / kernel/nvme.c drive
 * disks over their respective controllers, this drives the disk a USB flash
 * stick presents: enumerate a Mass-Storage / SCSI-transparent / Bulk-Only
 * interface (class 0x08, subclass 0x06, protocol 0x50) on the USB bus, find its
 * BULK IN + OUT endpoints, then issue SCSI READ CAPACITY / READ(10) inside the
 * BOT command/data/status wrapper sequence. QEMU's `-device usb-storage`
 * presents exactly this.
 *
 * This is an *additional*, read-focused block device (a stretch write path is
 * gated behind a clean round-trip). FAT32/VFS stay on the legacy ATA boot disk.
 * usb_storage_init() probes the USB bus for a mass-storage interface; if there's
 * none it is a clean no-op, so a machine without a USB stick boots unchanged and
 * the existing USB tablet path is untouched.
 */
#pragma once
#include <stdint.h>

#define USB_STORAGE_SECTOR_SIZE 512

/* Bring the UHCI controller up (shared with the tablet), enumerate a USB
 * mass-storage device on the bus, and READ CAPACITY it. Returns 0 if a usable
 * BOT/SCSI disk was found + sized, -1 if none is present (a clean no-op). */
int usb_storage_init(void);

/* 1 if usb_storage_init() brought a device up, else 0. */
int usb_storage_present(void);

/* Capacity of the device in 512-byte sectors (0 if no device). */
uint64_t usb_storage_capacity(void);

/* Read `count` 512-byte sectors starting at LBA `lba` into `buf` (must hold
 * count*512 bytes). Returns 0 on success, -1 on error/timeout/bad-arg. Bounds
 * the request to the device capacity and chunks large reads internally. */
int usb_storage_read(uint32_t lba, uint32_t count, void *buf);

/* Write `count` 512-byte sectors from `buf` to the USB disk at `lba` (SCSI
 * WRITE(10) over Bulk-Only Transport). Same bounds/chunking as the read. Returns
 * 0 on success, -1 otherwise. (M1728: wired into the block layer.) */
int usb_storage_write(uint32_t lba, uint32_t count, const void *buf);

/* Boot-time self-test: if a USB mass-storage device is present, log its
 * INQUIRY string + capacity, read a few sectors and log their bytes + an
 * additive checksum so the read can be matched against known on-disk content.
 * No-op (logs "none attached") if no device. */
void usb_storage_selftest(void);
