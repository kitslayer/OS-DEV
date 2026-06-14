/* ata.h — read sectors from the primary ATA disk (PIO mode). */
#pragma once
#include <stdint.h>

#define SECTOR_SIZE 512

/* Read `count` 512-byte sectors starting at LBA `lba` into `buf`.
 * Returns 0 on success, -1 on error/timeout. */
int ata_read(uint32_t lba, uint8_t count, void *buf);

/* Write `count` 512-byte sectors starting at LBA `lba` from `buf`. */
int ata_write(uint32_t lba, uint8_t count, const void *buf);
