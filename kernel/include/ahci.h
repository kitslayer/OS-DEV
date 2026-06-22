/*
 * ahci.h — AHCI/SATA disk driver (modern storage, DMA via an MMIO HBA).
 *
 * Unlike kernel/ata.c (legacy PIO, primary-master only — and the disk the OS
 * BOOTS from), this drives a SATA disk attached to an AHCI host bus adapter
 * (HBA) discovered on the PCI bus: PCI class 0x01, subclass 0x06, prog-IF 0x01.
 * Sector transfers are real DMA — the HBA reads/writes RAM itself via a command
 * list + command tables + a PRDT (physical-region descriptor table) we build.
 *
 * This is an *additional* block device; FAT32/VFS stay on the ATA boot disk.
 */
#pragma once
#include <stdint.h>

#define AHCI_SECTOR_SIZE 512

/* Probe the PCI bus for an AHCI HBA and bring up its ports. Returns the number
 * of usable SATA disks found (>=0), or -1 if no AHCI HBA is present. A config
 * without an AHCI controller is a clean no-op (returns -1), so the boot still
 * works on the legacy-ATA-only setup. */
int ahci_init(void);

/* How many SATA disks ahci_init() brought up (0 if none / not initialized). */
int ahci_disk_count(void);

/* Read `count` 512-byte sectors starting at 48-bit LBA `lba` from the SATA disk
 * `disk` (0..ahci_disk_count()-1) into `buf` (must hold count*512 bytes; the
 * HBA DMAs into its physical frame, so it must be in identity-mapped low RAM).
 * Returns 0 on success, -1 on error/timeout/bad-arg. */
int ahci_read(int disk, uint64_t lba, uint32_t count, void *buf);

/* Write `count` sectors from `buf` to `disk` at `lba` (WRITE DMA EXT). Same
 * buffer constraints as ahci_read. Returns 0 on success, -1 otherwise. */
int ahci_write(int disk, uint64_t lba, uint32_t count, const void *buf);

/* Boot-time self-test: if an AHCI disk is present, read sector 0 (and a couple
 * more) and log the first bytes + a checksum to the console/serial, so the read
 * path can be verified against known on-disk content. No-op if no AHCI disk. */
void ahci_selftest(void);
